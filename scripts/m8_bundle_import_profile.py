#!/usr/bin/env python3
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import sys
import tempfile
from typing import Any, Callable

from m8_bundle_common import (
    RECEIPT_SCHEMA,
    EvidenceInvalid,
    artifact_path,
    authority_source_hashes,
    canonical_json,
    clean_git_identity,
    exclusive_write,
    load_bundle_plan,
    receipt_path,
    sha256_bytes,
    sha256_file,
)
from m8_profile_case_provenance_verifier import EvidenceInvalid as ProvenanceEvidenceInvalid
from m8_profile_case_provenance_verifier import verify as provenance_verify
from m8_profile_collection_binder_v2 import EvidenceInvalid as CollectionEvidenceInvalid
from m8_profile_collection_binder_v2 import bind as profile_bind
from m8_profile_collection_binder import EvidenceInvalid as LegacyCollectionEvidenceInvalid
from zevryon_platform.performance_contract import DeviceClass

ATTEMPT_FILES = {
    "case": "profile-case.json",
    "provenance": "profile-case-provenance.json",
    "physical_host": "physical-host.json",
    "copy_output": "probe-work/copy-output.txt",
}
PROFILES = tuple(device.value for device in DeviceClass)
PROFILE_SUPPORT_PATHS = {
    "collection": "profile-support/profile-collection.json",
    **{f"{name}.case": f"profile-support/cases/{name}.json" for name in PROFILES},
    **{f"{name}.provenance": f"profile-support/provenance/{name}.json" for name in PROFILES},
    **{f"{name}.physical_host": f"profile-support/physical-host/{name}.json" for name in PROFILES},
    **{f"{name}.verification": f"profile-support/verifications/{name}.json" for name in PROFILES},
}


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")


def json_bytes(value: object) -> bytes:
    return (json.dumps(value, indent=2, sort_keys=True) + "\n").encode("utf-8")


def support_path(root: Path, key: str) -> Path:
    if key not in PROFILE_SUPPORT_PATHS:
        raise EvidenceInvalid(f"unknown profile support key: {key}")
    path = (root.resolve() / PROFILE_SUPPORT_PATHS[key]).resolve(strict=False)
    if not path.is_relative_to(root.resolve()):
        raise EvidenceInvalid(f"profile support path escaped artifact root: {key}")
    return path


def write_bytes_exclusive(path: Path, raw: bytes) -> None:
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("xb") as handle:
            handle.write(raw)
    except FileExistsError as exc:
        raise EvidenceInvalid(f"refusing to overwrite immutable profile evidence: {path}") from exc


def parse_attempts(values: list[str]) -> dict[DeviceClass, Path]:
    if len(values) != len(DeviceClass):
        raise EvidenceInvalid("exactly four --attempt DEVICE=DIRECTORY bindings are required")
    result: dict[DeviceClass, Path] = {}
    for value in values:
        if "=" not in value:
            raise EvidenceInvalid(f"attempt binding must use DEVICE=DIRECTORY: {value!r}")
        raw_device, raw_path = value.split("=", 1)
        try:
            device = DeviceClass(raw_device)
        except ValueError as exc:
            raise EvidenceInvalid(f"invalid attempt device class: {raw_device!r}") from exc
        if device in result:
            raise EvidenceInvalid(f"duplicate attempt binding for {device.value}")
        directory = Path(raw_path).expanduser().resolve()
        if not directory.is_dir():
            raise EvidenceInvalid(f"attempt directory is missing for {device.value}: {directory}")
        result[device] = directory
    if set(result) != set(DeviceClass):
        raise EvidenceInvalid("attempt profile set is incomplete")
    return result


def attempt_file(directory: Path, key: str) -> Path:
    path = (directory.resolve() / ATTEMPT_FILES[key]).resolve()
    if not path.is_relative_to(directory.resolve()) or not path.is_file():
        raise EvidenceInvalid(f"attempt file is missing or escaped: {path}")
    return path


def check_verification(value: dict[str, Any], device: DeviceClass, plan: dict[str, Any]) -> None:
    expected = (
        value.get("schema") == "zevryon.m8.profile-case-provenance-verification.v1"
        and value.get("authority") == "m8-profile-case-provenance-verifier-v1"
        and value.get("provenance_gate_passed") is True
        and value.get("device_class") == device.value
        and value.get("candidate_commit") == plan["candidate_commit"]
        and value.get("candidate_tree") == plan["candidate_tree"]
        and isinstance(value.get("case_id"), str)
        and bool(value["case_id"])
    )
    if not expected:
        raise EvidenceInvalid(f"{device.value} provenance verification identity mismatch")


def perform_import(*, artifact_root: Path, titan_report: Path, titan: Path, probe: Path, attempts: dict[DeviceClass, Path], verifier: Callable[..., dict[str, Any]] = provenance_verify, binder: Callable[..., tuple[dict[str, Any], dict[str, Any], bool]] = profile_bind) -> tuple[int, dict[str, Any]]:
    root = artifact_root.resolve()
    plan, _ = load_bundle_plan(root, verify_current=True)
    output = artifact_path(root, plan, "profile")
    receipt = receipt_path(root, plan, "profile")
    support = {"collection": support_path(root, "collection")}
    for device in DeviceClass:
        for suffix in ("case", "provenance", "physical_host", "verification"):
            key = f"{device.value}.{suffix}"
            support[key] = support_path(root, key)
    for path in (output, receipt, *support.values()):
        if path.exists():
            raise EvidenceInvalid(f"profile import destination already exists: {path}")

    titan_report, titan, probe = map(lambda p: p.resolve(), (titan_report, titan, probe))
    for path, label in ((titan_report, "Titan report"), (titan, "Titan"), (probe, "probe")):
        if not path.is_file():
            raise EvidenceInvalid(f"{label} is missing: {path}")

    staged: dict[str, bytes] = {}
    verifications: dict[DeviceClass, dict[str, Any]] = {}
    case_paths: dict[DeviceClass, Path] = {}
    verification_paths: dict[DeviceClass, Path] = {}
    copy_hashes: dict[str, str] = {}
    with tempfile.TemporaryDirectory(prefix="zevryon-m8-profile-import-") as tmp:
        tmp_root = Path(tmp)
        for device in DeviceClass:
            directory = attempts[device].resolve()
            paths = {key: attempt_file(directory, key) for key in ATTEMPT_FILES}
            verification = verifier(provenance_path=paths["provenance"], case_path=paths["case"], host_path=paths["physical_host"], titan_report_path=titan_report, titan_path=titan, copy_output_path=paths["copy_output"], probe_path=probe)
            if not isinstance(verification, dict):
                raise EvidenceInvalid(f"{device.value} verifier did not return an object")
            check_verification(verification, device, plan)
            verification_raw = json_bytes(verification)
            verification_path = tmp_root / f"{device.value}.json"
            verification_path.write_bytes(verification_raw)
            case_paths[device] = paths["case"]
            verification_paths[device] = verification_path
            verifications[device] = verification
            copy_hashes[device.value] = sha256_file(paths["copy_output"])
            staged[f"{device.value}.case"] = paths["case"].read_bytes()
            staged[f"{device.value}.provenance"] = paths["provenance"].read_bytes()
            staged[f"{device.value}.physical_host"] = paths["physical_host"].read_bytes()
            staged[f"{device.value}.verification"] = verification_raw
        observations, collection, gate_passed = binder(titan_path=titan_report, case_paths=case_paths, verification_paths=verification_paths)

    if not isinstance(observations, dict) or not isinstance(collection, dict):
        raise EvidenceInvalid("profile binder returned malformed objects")
    for document, label in ((observations, "observations"), (collection, "collection")):
        if document.get("candidate_commit") != plan["candidate_commit"] or document.get("candidate_tree") != plan["candidate_tree"]:
            raise EvidenceInvalid(f"profile {label} candidate binding mismatch")
    if collection.get("schema") != "zevryon.m8.profile-collection.v2" or collection.get("authority") != "m8-four-profile-collection-binder-v2" or collection.get("all_provenance_receipts_verified") is not True or collection.get("verified_provenance_receipt_count") != 4:
        raise EvidenceInvalid("profile collection provenance gate mismatch")

    observations_raw, collection_raw = json_bytes(observations), json_bytes(collection)
    if collection.get("observations_sha256") != sha256_bytes(observations_raw):
        raise EvidenceInvalid("profile observations serialization SHA-256 drifted")
    staged["collection"] = collection_raw
    if clean_git_identity() != (plan["candidate_commit"], plan["candidate_tree"]):
        raise EvidenceInvalid("candidate changed during profile import")
    if authority_source_hashes() != plan["authority_source_sha256"]:
        raise EvidenceInvalid("authority sources changed during profile import")

    write_bytes_exclusive(output, observations_raw)
    for key, raw in staged.items():
        write_bytes_exclusive(support[key], raw)
    if clean_git_identity() != (plan["candidate_commit"], plan["candidate_tree"]):
        raise EvidenceInvalid("candidate changed while profile evidence was sealed")
    if authority_source_hashes() != plan["authority_source_sha256"]:
        raise EvidenceInvalid("authority sources changed while profile evidence was sealed")

    support_hashes = {key: sha256_bytes(raw) for key, raw in sorted(staged.items())}
    receipt_document = {
        "schema": RECEIPT_SCHEMA,
        "authority": "m8-profile-attempt-import-v2",
        "kind": "profile-attempt-import-v2",
        "bundle_id": plan["bundle_id"],
        "candidate_commit": plan["candidate_commit"],
        "candidate_tree": plan["candidate_tree"],
        "artifact_key": "profile",
        "artifact_path": plan["artifacts"]["profile"],
        "receipt_path": plan["receipts"]["profile"],
        "imported_utc": utc_now(),
        "artifact_exists": True,
        "artifact_sha256": sha256_bytes(observations_raw),
        "artifact_bytes": len(observations_raw),
        "profile_support_paths": dict(PROFILE_SUPPORT_PATHS),
        "profile_support_sha256": support_hashes,
        "external_artifacts": {
            "titan_report": {"path": str(titan_report), "sha256": sha256_file(titan_report)},
            "titan_container": {"path": str(titan), "sha256": sha256_file(titan)},
            "profile_probe": {"path": str(probe), "sha256": sha256_file(probe)},
            "copy_output_sha256": dict(sorted(copy_hashes.items())),
        },
        "verifier_receipts": {device.value: {"case_id": verifications[device]["case_id"], "sha256": support_hashes[f"{device.value}.verification"]} for device in DeviceClass},
        "candidate_unchanged": True,
        "authority_sources_unchanged": True,
        "evidence_valid": True,
        "recomputed_gate_passed": bool(gate_passed),
        "collection_schema": collection["schema"],
        "collection_authority": collection["authority"],
        "collection_sha256": support_hashes["collection"],
    }
    exclusive_write(receipt, canonical_json(receipt_document))
    return (0 if gate_passed else 2), receipt_document


def main() -> int:
    parser = argparse.ArgumentParser(description="Import four independently verified physical profile attempts into one frozen M8 bundle.")
    parser.add_argument("--artifact-root", type=Path, required=True)
    parser.add_argument("--titan-report", type=Path, required=True)
    parser.add_argument("--titan", type=Path, required=True)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--attempt", action="append", required=True, metavar="DEVICE=DIRECTORY")
    args = parser.parse_args()
    root = args.artifact_root.resolve()
    try:
        attempts = parse_attempts(args.attempt)
        code, _ = perform_import(artifact_root=root, titan_report=args.titan_report, titan=args.titan, probe=args.probe, attempts=attempts)
        return code
    except (EvidenceInvalid, ProvenanceEvidenceInvalid, CollectionEvidenceInvalid, LegacyCollectionEvidenceInvalid, OSError, ValueError, TypeError, KeyError) as exc:
        try:
            plan, _ = load_bundle_plan(root, verify_current=False)
            receipt = receipt_path(root, plan, "profile")
            if not receipt.exists():
                failure = {
                    "schema": RECEIPT_SCHEMA,
                    "authority": "m8-profile-attempt-import-v2",
                    "kind": "profile-attempt-import-v2",
                    "bundle_id": plan["bundle_id"],
                    "candidate_commit": plan["candidate_commit"],
                    "candidate_tree": plan["candidate_tree"],
                    "artifact_key": "profile",
                    "artifact_path": plan["artifacts"]["profile"],
                    "receipt_path": plan["receipts"]["profile"],
                    "imported_utc": utc_now(),
                    "artifact_exists": False,
                    "evidence_valid": False,
                    "recomputed_gate_passed": False,
                    "profile_support_paths": dict(PROFILE_SUPPORT_PATHS),
                    "error": str(exc),
                }
                exclusive_write(receipt, canonical_json(failure))
        except (EvidenceInvalid, OSError):
            pass
        print(f"M8 profile bundle import v2 failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
