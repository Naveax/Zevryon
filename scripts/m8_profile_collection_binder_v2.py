#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys
from typing import Any

SOURCE_ROOT = Path(__file__).resolve().parents[1]
if str(SOURCE_ROOT) not in sys.path:
    sys.path.insert(0, str(SOURCE_ROOT))

from scripts import m8_profile_collection_binder as legacy  # noqa: E402
from zevryon_platform.performance_contract import DeviceClass  # noqa: E402

VERIFY_SCHEMA = "zevryon.m8.profile-case-provenance-verification.v1"
VERIFY_AUTHORITY = "m8-profile-case-provenance-verifier-v1"
COLLECTION_SCHEMA = "zevryon.m8.profile-collection.v2"
COLLECTION_AUTHORITY = "m8-four-profile-collection-binder-v2"
HEX40 = re.compile(r"^[0-9a-f]{40}$")
HEX64 = re.compile(r"^[0-9a-f]{64}$")


class EvidenceInvalid(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise EvidenceInvalid(message)


def sha256_bytes(raw: bytes) -> str:
    return hashlib.sha256(raw).hexdigest()


def load_object(path: Path, label: str) -> tuple[dict[str, Any], bytes]:
    require(path.is_file(), f"{label} file is missing: {path}")
    raw = path.read_bytes()
    try:
        text = raw.decode("utf-8", errors="strict")
        value = json.loads(
            text,
            parse_constant=lambda token: (_ for _ in ()).throw(
                EvidenceInvalid(f"{label} contains non-finite JSON constant: {token}")
            ),
        )
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise EvidenceInvalid(f"{label} is not strict UTF-8 JSON: {exc}") from exc
    require(isinstance(value, dict), f"{label} must be a JSON object")
    return value, raw


def hex40(value: object, label: str) -> str:
    require(isinstance(value, str) and HEX40.fullmatch(value) is not None, f"{label} must be lowercase 40-hex")
    return value


def hex64(value: object, label: str) -> str:
    require(isinstance(value, str) and HEX64.fullmatch(value) is not None, f"{label} must be lowercase 64-hex")
    return value


def parse_profile_paths(values: list[str], label: str) -> dict[DeviceClass, Path]:
    expected = set(DeviceClass)
    require(len(values) == len(expected), f"exactly four {label} bindings are required")
    result: dict[DeviceClass, Path] = {}
    for value in values:
        require("=" in value, f"{label} binding must use DEVICE=PATH: {value!r}")
        raw_device, raw_path = value.split("=", 1)
        try:
            device = DeviceClass(raw_device)
        except ValueError as exc:
            raise EvidenceInvalid(f"invalid {label} device class: {raw_device!r}") from exc
        require(device not in result, f"duplicate {label} binding for {device.value}")
        require(raw_path.strip() != "", f"{label} path is empty for {device.value}")
        path = Path(raw_path).expanduser().resolve()
        require(path.is_file(), f"{label} file is missing for {device.value}: {path}")
        result[device] = path
    require(set(result) == expected, f"{label} profile set is incomplete")
    return result


def validate_verification(
    *,
    device: DeviceClass,
    case_path: Path,
    verification_path: Path,
    titan: dict[str, Any],
    titan_report_sha256: str,
) -> dict[str, Any]:
    case, case_raw = load_object(case_path, f"profile case {device.value}")
    verification, verification_raw = load_object(verification_path, f"provenance verification {device.value}")

    require(verification.get("schema") == VERIFY_SCHEMA, f"{device.value} verification schema mismatch")
    require(verification.get("authority") == VERIFY_AUTHORITY, f"{device.value} verification authority mismatch")
    require(verification.get("provenance_gate_passed") is True, f"{device.value} provenance gate did not pass")

    commit = hex40(verification.get("candidate_commit"), f"{device.value} verification candidate_commit")
    tree = hex40(verification.get("candidate_tree"), f"{device.value} verification candidate_tree")
    require(commit == case.get("candidate_commit") == titan.get("candidate_commit"), f"{device.value} candidate commit binding drifted")
    require(tree == case.get("candidate_tree") == titan.get("candidate_tree"), f"{device.value} candidate tree binding drifted")
    require(verification.get("device_class") == device.value, f"{device.value} verification device class mismatch")
    require(case.get("device_class") == device.value, f"{device.value} case device class mismatch")
    require(isinstance(case.get("case_id"), str) and case.get("case_id"), f"{device.value} case_id is missing")
    require(verification.get("case_id") == case.get("case_id"), f"{device.value} case_id binding drifted")

    artifacts = verification.get("artifacts")
    require(isinstance(artifacts, dict), f"{device.value} verification artifacts block is missing")
    expected_artifacts = {
        "provenance_sha256",
        "case_sha256",
        "physical_host_sha256",
        "titan_report_sha256",
        "titan_container_sha256",
        "copy_output_sha256",
        "probe_sha256",
    }
    require(set(artifacts) == expected_artifacts, f"{device.value} verification artifact fields drifted")
    for name in sorted(expected_artifacts):
        hex64(artifacts.get(name), f"{device.value} verification artifacts.{name}")

    actual_case_sha = sha256_bytes(case_raw)
    require(artifacts["case_sha256"] == actual_case_sha, f"{device.value} verified case SHA-256 mismatch")
    require(artifacts["titan_report_sha256"] == titan_report_sha256, f"{device.value} verified Titan report SHA-256 mismatch")

    generation = titan.get("generation")
    require(isinstance(generation, dict), "Titan generation block is missing")
    container_sha = hex64(generation.get("container_sha256"), "Titan container_sha256")
    payload_sha = hex64(generation.get("payload_sha256"), "Titan payload_sha256")
    require(artifacts["titan_container_sha256"] == container_sha, f"{device.value} verified Titan container SHA-256 mismatch")
    require(artifacts["copy_output_sha256"] == payload_sha, f"{device.value} verified copy-output SHA-256 mismatch")

    host = case.get("physical_host")
    require(isinstance(host, dict), f"{device.value} case physical_host block is missing")
    host_sha = hex64(host.get("receipt_sha256"), f"{device.value} physical host receipt SHA-256")
    require(artifacts["physical_host_sha256"] == host_sha, f"{device.value} verified physical-host SHA-256 mismatch")

    host_summary = verification.get("physical_host")
    require(isinstance(host_summary, dict), f"{device.value} verified physical-host summary is missing")
    require(host_summary.get("before_and_after_recertified") is True, f"{device.value} host was not independently recertified")
    require(type(host_summary.get("physical_ram_mib")) is int and host_summary["physical_ram_mib"] > 0, f"{device.value} verified physical RAM is invalid")
    require(host_summary.get("physical_ram_mib") == host.get("physical_memory_mib"), f"{device.value} verified physical RAM differs from case")
    require(isinstance(host_summary.get("system_fingerprint"), str) and host_summary["system_fingerprint"], f"{device.value} verified host fingerprint is missing")

    pss = verification.get("pss")
    require(isinstance(pss, dict), f"{device.value} verified PSS summary is missing")
    require(type(pss.get("sample_count")) is int and pss["sample_count"] > 0, f"{device.value} verified PSS sample count is invalid")
    require(type(pss.get("root_pid")) is int and pss["root_pid"] > 0, f"{device.value} verified root PID is invalid")
    require(type(pss.get("maximum_aggregate_pss_mb")) in {int, float} and float(pss["maximum_aggregate_pss_mb"]) >= 0.0, f"{device.value} verified maximum PSS is invalid")

    phases = verification.get("phases")
    require(isinstance(phases, dict), f"{device.value} verified phase summary is missing")
    require(phases.get("layout_store_read_policy_applied") is True, f"{device.value} layout StoreReadConfig was not verified")
    require(phases.get("scroll_samples") == 257, f"{device.value} verified scroll sample count drifted")
    require(phases.get("mutation_samples") == 257, f"{device.value} verified mutation sample count drifted")

    return {
        "case_id": case["case_id"],
        "device_class": device.value,
        "case_path": str(case_path),
        "case_sha256": actual_case_sha,
        "verification_path": str(verification_path),
        "verification_sha256": sha256_bytes(verification_raw),
        "provenance_sha256": artifacts["provenance_sha256"],
        "physical_host_sha256": artifacts["physical_host_sha256"],
        "titan_report_sha256": artifacts["titan_report_sha256"],
        "titan_container_sha256": artifacts["titan_container_sha256"],
        "copy_output_sha256": artifacts["copy_output_sha256"],
        "probe_sha256": artifacts["probe_sha256"],
        "provenance_gate_passed": True,
    }


def bind(
    *,
    titan_path: Path,
    case_paths: dict[DeviceClass, Path],
    verification_paths: dict[DeviceClass, Path],
) -> tuple[dict[str, Any], dict[str, Any], bool]:
    require(set(case_paths) == set(DeviceClass), "case profile set is incomplete")
    require(set(verification_paths) == set(DeviceClass), "verification profile set is incomplete")

    titan, titan_raw, titan_sha = legacy.load_titan(titan_path)
    verification_receipts: list[dict[str, Any]] = []
    ordered_cases: list[Path] = []
    for device in DeviceClass:
        case_path = case_paths[device]
        ordered_cases.append(case_path)
        verification_receipts.append(
            validate_verification(
                device=device,
                case_path=case_path,
                verification_path=verification_paths[device],
                titan=titan,
                titan_report_sha256=titan_sha,
            )
        )

    observations, legacy_collection, gate_passed = legacy.bind(titan_path, ordered_cases)
    verification_receipts.sort(key=lambda item: item["device_class"])

    collection = dict(legacy_collection)
    collection["schema"] = COLLECTION_SCHEMA
    collection["authority"] = COLLECTION_AUTHORITY
    collection["legacy_collection_schema"] = legacy_collection["schema"]
    collection["legacy_collection_authority"] = legacy_collection["authority"]
    collection["verified_provenance_receipts"] = verification_receipts
    collection["verified_provenance_receipt_count"] = len(verification_receipts)
    collection["all_provenance_receipts_verified"] = True
    collection["titan_report_sha256"] = sha256_bytes(titan_raw)
    return observations, collection, gate_passed


def write_exclusive(path: Path, value: object) -> bytes:
    path.parent.mkdir(parents=True, exist_ok=True)
    raw = (json.dumps(value, indent=2, sort_keys=True) + "\n").encode("utf-8")
    try:
        with path.open("xb") as handle:
            handle.write(raw)
    except FileExistsError as exc:
        raise EvidenceInvalid(f"refusing to overwrite output: {path}") from exc
    return raw


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Bind four raw M8 physical profile cases only after matching independent provenance-verification receipts pass."
    )
    parser.add_argument("--titan-report", type=Path, required=True)
    parser.add_argument("--case", action="append", required=True, metavar="DEVICE=PATH")
    parser.add_argument("--verification", action="append", required=True, metavar="DEVICE=PATH")
    parser.add_argument("--observations-output", type=Path, required=True)
    parser.add_argument("--receipt-output", type=Path, required=True)
    args = parser.parse_args()

    try:
        require(args.observations_output.resolve() != args.receipt_output.resolve(), "binder outputs must differ")
        case_paths = parse_profile_paths(args.case, "case")
        verification_paths = parse_profile_paths(args.verification, "verification")
        observations, collection, gate_passed = bind(
            titan_path=args.titan_report.resolve(),
            case_paths=case_paths,
            verification_paths=verification_paths,
        )
        observation_raw = write_exclusive(args.observations_output.resolve(), observations)
        require(collection["observations_sha256"] == sha256_bytes(observation_raw), "observation serialization SHA-256 drifted")
        write_exclusive(args.receipt_output.resolve(), collection)
        return 0 if gate_passed else 2
    except (EvidenceInvalid, legacy.EvidenceInvalid, OSError, ValueError) as exc:
        print(f"M8 provenance-aware profile collection binder failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
