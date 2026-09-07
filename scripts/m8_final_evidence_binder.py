#!/usr/bin/env python3
from __future__ import annotations

import json
from pathlib import Path
from typing import Any

import m8_final_evidence_binder_impl as implementation
from m8_bundle_common import EvidenceInvalid, RECEIPT_SCHEMA, sha256_bytes
from m8_bundle_import_profile import PROFILE_SUPPORT_PATHS
from m8_profile_observation_gate import EvidenceInvalid as ProfileEvidenceInvalid

_original_gate = implementation.gate
_original_validate_profile = implementation.validate_profile
_original_load_bundle_plan = implementation.load_bundle_plan
_original_exclusive_write = implementation.exclusive_write
_current_root: Path | None = None
_current_plan: dict[str, Any] | None = None
_current_plan_raw: bytes | None = None


def _strict_gate(condition: bool, message: str) -> None:
    if not condition and message.startswith("required raw artifact is missing:"):
        raise EvidenceInvalid(message)
    _original_gate(condition, message)


def _strict_validate_profile(raw: bytes, plan: dict[str, Any]) -> dict[str, Any]:
    try:
        return _original_validate_profile(raw, plan)
    except ProfileEvidenceInvalid as exc:
        raise EvidenceInvalid(f"profile evidence invalid: {exc}") from exc


def _capture_root(artifact_root: Path, verify_current: bool = True):
    global _current_root, _current_plan, _current_plan_raw
    _current_root = Path(artifact_root).resolve()
    _current_plan = None
    _current_plan_raw = None
    plan, raw = _original_load_bundle_plan(artifact_root, verify_current=verify_current)
    _current_plan = plan
    _current_plan_raw = raw
    return plan, raw


def _load_json(raw: bytes, label: str) -> dict[str, Any]:
    try:
        value = json.loads(raw.decode("utf-8", errors="strict"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise EvidenceInvalid(f"{label} is not strict UTF-8 JSON: {exc}") from exc
    if not isinstance(value, dict):
        raise EvidenceInvalid(f"{label} must be an object")
    return value


def _support_file(key: str) -> tuple[Path, bytes]:
    if _current_root is None:
        raise EvidenceInvalid("profile receipt validation lost artifact-root context")
    if key not in PROFILE_SUPPORT_PATHS:
        raise EvidenceInvalid(f"unknown profile support key: {key}")
    path = (_current_root / PROFILE_SUPPORT_PATHS[key]).resolve()
    if not path.is_relative_to(_current_root):
        raise EvidenceInvalid(f"profile support path escaped artifact root: {key}")
    if not path.is_file():
        raise EvidenceInvalid(f"profile support file is missing: {key}")
    return path, path.read_bytes()


def _strict_validate_profile_receipt(value: Any, plan: dict[str, Any], raw: bytes) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise EvidenceInvalid("profile receipt must be an object")
    expected = {
        "schema": RECEIPT_SCHEMA,
        "authority": "m8-profile-attempt-import-v2",
        "kind": "profile-attempt-import-v2",
        "bundle_id": plan["bundle_id"],
        "candidate_commit": plan["candidate_commit"],
        "candidate_tree": plan["candidate_tree"],
        "artifact_key": "profile",
        "artifact_path": plan["artifacts"]["profile"],
        "receipt_path": plan["receipts"]["profile"],
    }
    for key, wanted in expected.items():
        if value.get(key) != wanted:
            raise EvidenceInvalid(f"profile receipt {key} mismatch")
    for key in ("candidate_unchanged", "authority_sources_unchanged", "evidence_valid", "artifact_exists"):
        if value.get(key) is not True:
            raise EvidenceInvalid(f"profile receipt {key} is not true")
    if value.get("artifact_sha256") != sha256_bytes(raw) or value.get("artifact_bytes") != len(raw):
        raise EvidenceInvalid("profile receipt observation artifact hash/size mismatch")
    if value.get("profile_support_paths") != PROFILE_SUPPORT_PATHS:
        raise EvidenceInvalid("profile support path contract drifted")

    support_hashes = value.get("profile_support_sha256")
    if not isinstance(support_hashes, dict) or set(support_hashes) != set(PROFILE_SUPPORT_PATHS):
        raise EvidenceInvalid("profile support SHA-256 set drifted")
    support_docs: dict[str, dict[str, Any]] = {}
    for key in PROFILE_SUPPORT_PATHS:
        _, support_raw = _support_file(key)
        if support_hashes.get(key) != sha256_bytes(support_raw):
            raise EvidenceInvalid(f"profile support SHA-256 mismatch: {key}")
        support_docs[key] = _load_json(support_raw, f"profile support {key}")

    collection = support_docs["collection"]
    if collection.get("schema") != "zevryon.m8.profile-collection.v2" or collection.get("authority") != "m8-four-profile-collection-binder-v2":
        raise EvidenceInvalid("profile collection support receipt identity mismatch")
    if collection.get("candidate_commit") != plan["candidate_commit"] or collection.get("candidate_tree") != plan["candidate_tree"]:
        raise EvidenceInvalid("profile collection support candidate mismatch")
    if collection.get("observations_sha256") != sha256_bytes(raw):
        raise EvidenceInvalid("profile collection observations SHA-256 mismatch")
    if collection.get("all_provenance_receipts_verified") is not True or collection.get("verified_provenance_receipt_count") != 4:
        raise EvidenceInvalid("profile collection provenance summary mismatch")
    verified = collection.get("verified_provenance_receipts")
    if not isinstance(verified, list) or len(verified) != 4:
        raise EvidenceInvalid("profile collection verified receipt array mismatch")
    collection_by_device = {item.get("device_class"): item for item in verified if isinstance(item, dict)}
    devices = {"legacy-phone", "mid-phone", "modern-phone", "desktop"}
    if set(collection_by_device) != devices:
        raise EvidenceInvalid("profile collection device set mismatch")

    external = value.get("external_artifacts")
    if not isinstance(external, dict):
        raise EvidenceInvalid("profile receipt external artifact binding is missing")
    for name in ("titan_report", "titan_container", "profile_probe"):
        block = external.get(name)
        if not isinstance(block, dict) or not isinstance(block.get("sha256"), str):
            raise EvidenceInvalid(f"profile external binding is invalid: {name}")
    copy_hashes = external.get("copy_output_sha256")
    if not isinstance(copy_hashes, dict) or set(copy_hashes) != devices:
        raise EvidenceInvalid("profile copy-output hash set mismatch")
    receipt_verifiers = value.get("verifier_receipts")
    if not isinstance(receipt_verifiers, dict) or set(receipt_verifiers) != devices:
        raise EvidenceInvalid("profile verifier receipt set mismatch")

    for device, collection_item in collection_by_device.items():
        verification = support_docs[f"{device}.verification"]
        case = support_docs[f"{device}.case"]
        if verification.get("schema") != "zevryon.m8.profile-case-provenance-verification.v1" or verification.get("authority") != "m8-profile-case-provenance-verifier-v1" or verification.get("provenance_gate_passed") is not True or verification.get("device_class") != device or verification.get("candidate_commit") != plan["candidate_commit"] or verification.get("candidate_tree") != plan["candidate_tree"]:
            raise EvidenceInvalid(f"profile verification identity mismatch: {device}")
        if case.get("device_class") != device or case.get("case_id") != verification.get("case_id"):
            raise EvidenceInvalid(f"profile case binding mismatch: {device}")
        artifacts = verification.get("artifacts")
        if not isinstance(artifacts, dict):
            raise EvidenceInvalid(f"profile verification artifact block missing: {device}")
        expected_links = {
            "case_sha256": support_hashes[f"{device}.case"],
            "provenance_sha256": support_hashes[f"{device}.provenance"],
            "physical_host_sha256": support_hashes[f"{device}.physical_host"],
            "titan_report_sha256": external["titan_report"]["sha256"],
            "titan_container_sha256": external["titan_container"]["sha256"],
            "probe_sha256": external["profile_probe"]["sha256"],
            "copy_output_sha256": copy_hashes[device],
        }
        for field, wanted in expected_links.items():
            if artifacts.get(field) != wanted:
                raise EvidenceInvalid(f"profile verification {field} mismatch: {device}")
        if collection_item.get("case_id") != verification.get("case_id"):
            raise EvidenceInvalid(f"profile collection case_id mismatch: {device}")
        if collection_item.get("verification_sha256") != support_hashes[f"{device}.verification"]:
            raise EvidenceInvalid(f"profile collection verification SHA mismatch: {device}")
        if receipt_verifiers[device].get("case_id") != verification.get("case_id") or receipt_verifiers[device].get("sha256") != support_hashes[f"{device}.verification"]:
            raise EvidenceInvalid(f"profile importer verifier receipt mismatch: {device}")

    if value.get("collection_schema") != collection["schema"] or value.get("collection_authority") != collection["authority"]:
        raise EvidenceInvalid("profile importer collection identity mismatch")
    if value.get("collection_sha256") != support_hashes["collection"]:
        raise EvidenceInvalid("profile importer collection SHA mismatch")
    implementation.gate(value.get("recomputed_gate_passed") is True, "profile import recorded a valid four-profile gate failure")
    return value


def _strict_exclusive_write(path: Path, text: str) -> None:
    if _current_root is not None and _current_plan is not None and _current_plan_raw is not None:
        final_path = implementation.resolve_contained(_current_root, _current_plan["final_output"], "final output")
        if Path(path).resolve(strict=False) == final_path:
            try:
                value = json.loads(text)
            except json.JSONDecodeError as exc:
                raise EvidenceInvalid(f"final decision is not valid JSON: {exc}") from exc
            if not isinstance(value, dict):
                raise EvidenceInvalid("final decision must be a JSON object")
            if value.get("schema") == implementation.FINAL_SCHEMA and value.get("gate_passed") is False:
                bindings = {
                    "bundle_id": _current_plan["bundle_id"],
                    "candidate_commit": _current_plan["candidate_commit"],
                    "candidate_tree": _current_plan["candidate_tree"],
                    "bundle_plan_sha256": sha256_bytes(_current_plan_raw),
                }
                enriched = dict(value)
                for key, wanted in bindings.items():
                    if key in enriched and enriched[key] != wanted:
                        raise EvidenceInvalid(f"final decision {key} conflicts with frozen bundle identity")
                    enriched[key] = wanted
                text = implementation.canonical_json(enriched)
    _original_exclusive_write(path, text)


def main() -> int:
    implementation.gate = _strict_gate
    implementation.validate_profile = _strict_validate_profile
    implementation.load_bundle_plan = _capture_root
    implementation.validate_profile_receipt = _strict_validate_profile_receipt
    implementation.exclusive_write = _strict_exclusive_write
    return implementation.main()


if __name__ == "__main__":
    raise SystemExit(main())
