#!/usr/bin/env python3
from __future__ import annotations

import copy
import hashlib
import json
from pathlib import Path
import tempfile

import m8_final_evidence_binder as subject
from m8_bundle_common import ARTIFACT_PATHS, RECEIPT_PATHS, RECEIPT_SCHEMA, EvidenceInvalid

COMMIT, TREE = "1" * 40, "2" * 40
DEVICES = ("legacy-phone", "mid-phone", "modern-phone", "desktop")


def raw(value: object) -> bytes:
    return (json.dumps(value, indent=2, sort_keys=True) + "\n").encode()


def write(root: Path, key: str, value: object) -> bytes:
    data = raw(value)
    path = root / subject.PROFILE_SUPPORT_PATHS[key]
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return data


def make_fixture(root: Path):
    observations = raw({"candidate_commit": COMMIT, "candidate_tree": TREE})
    external = {
        "titan_report": {"path": "/external/titan.json", "sha256": "a" * 64},
        "titan_container": {"path": "/external/titan.zmdoc", "sha256": "b" * 64},
        "profile_probe": {"path": "/external/probe", "sha256": "c" * 64},
        "copy_output_sha256": {device: chr(ord("d") + index) * 64 for index, device in enumerate(DEVICES)},
    }
    support_hashes = {}
    verified = []
    verifier_receipts = {}
    for index, device in enumerate(DEVICES):
        case = {"candidate_commit": COMMIT, "candidate_tree": TREE, "device_class": device, "case_id": f"case-{index}"}
        case_raw = write(root, f"{device}.case", case)
        provenance_raw = write(root, f"{device}.provenance", {"p": index})
        host_raw = write(root, f"{device}.physical_host", {"h": index})
        verification = {
            "schema": "zevryon.m8.profile-case-provenance-verification.v1",
            "authority": "m8-profile-case-provenance-verifier-v1",
            "candidate_commit": COMMIT,
            "candidate_tree": TREE,
            "device_class": device,
            "case_id": case["case_id"],
            "provenance_gate_passed": True,
            "artifacts": {
                "case_sha256": hashlib.sha256(case_raw).hexdigest(),
                "provenance_sha256": hashlib.sha256(provenance_raw).hexdigest(),
                "physical_host_sha256": hashlib.sha256(host_raw).hexdigest(),
                "titan_report_sha256": external["titan_report"]["sha256"],
                "titan_container_sha256": external["titan_container"]["sha256"],
                "probe_sha256": external["profile_probe"]["sha256"],
                "copy_output_sha256": external["copy_output_sha256"][device],
            },
        }
        verification_raw = write(root, f"{device}.verification", verification)
        verification_sha = hashlib.sha256(verification_raw).hexdigest()
        support_hashes.update({
            f"{device}.case": hashlib.sha256(case_raw).hexdigest(),
            f"{device}.provenance": hashlib.sha256(provenance_raw).hexdigest(),
            f"{device}.physical_host": hashlib.sha256(host_raw).hexdigest(),
            f"{device}.verification": verification_sha,
        })
        verified.append({"device_class": device, "case_id": case["case_id"], "verification_sha256": verification_sha})
        verifier_receipts[device] = {"case_id": case["case_id"], "sha256": verification_sha}

    collection = {
        "schema": "zevryon.m8.profile-collection.v2",
        "authority": "m8-four-profile-collection-binder-v2",
        "candidate_commit": COMMIT,
        "candidate_tree": TREE,
        "observations_sha256": hashlib.sha256(observations).hexdigest(),
        "all_provenance_receipts_verified": True,
        "verified_provenance_receipt_count": 4,
        "verified_provenance_receipts": verified,
    }
    collection_raw = write(root, "collection", collection)
    support_hashes["collection"] = hashlib.sha256(collection_raw).hexdigest()
    plan = {"bundle_id": "4" * 32, "candidate_commit": COMMIT, "candidate_tree": TREE, "artifacts": dict(ARTIFACT_PATHS), "receipts": dict(RECEIPT_PATHS)}
    receipt = {
        "schema": RECEIPT_SCHEMA,
        "authority": "m8-profile-attempt-import-v2",
        "kind": "profile-attempt-import-v2",
        "bundle_id": plan["bundle_id"],
        "candidate_commit": COMMIT,
        "candidate_tree": TREE,
        "artifact_key": "profile",
        "artifact_path": ARTIFACT_PATHS["profile"],
        "receipt_path": RECEIPT_PATHS["profile"],
        "artifact_exists": True,
        "artifact_sha256": hashlib.sha256(observations).hexdigest(),
        "artifact_bytes": len(observations),
        "profile_support_paths": dict(subject.PROFILE_SUPPORT_PATHS),
        "profile_support_sha256": support_hashes,
        "external_artifacts": external,
        "verifier_receipts": verifier_receipts,
        "candidate_unchanged": True,
        "authority_sources_unchanged": True,
        "evidence_valid": True,
        "recomputed_gate_passed": True,
        "collection_schema": collection["schema"],
        "collection_authority": collection["authority"],
        "collection_sha256": support_hashes["collection"],
    }
    return observations, plan, receipt


def test_valid_and_tamper() -> None:
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        observations, plan, receipt = make_fixture(root)
        subject._current_root = root.resolve()
        assert subject._strict_validate_profile_receipt(receipt, plan, observations) is receipt
        target = root / subject.PROFILE_SUPPORT_PATHS["legacy-phone.provenance"]
        target.write_bytes(target.read_bytes() + b" ")
        try:
            subject._strict_validate_profile_receipt(receipt, plan, observations)
        except EvidenceInvalid:
            pass
        else:
            raise AssertionError("support-file tamper must be evidence-invalid")


def test_external_and_candidate_binding() -> None:
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        observations, plan, receipt = make_fixture(root)
        subject._current_root = root.resolve()
        bad = copy.deepcopy(receipt)
        bad["external_artifacts"]["titan_container"]["sha256"] = "f" * 64
        try:
            subject._strict_validate_profile_receipt(bad, plan, observations)
        except EvidenceInvalid:
            pass
        else:
            raise AssertionError("Titan external hash tamper must fail")
        bad = copy.deepcopy(receipt)
        bad["candidate_tree"] = "f" * 40
        try:
            subject._strict_validate_profile_receipt(bad, plan, observations)
        except EvidenceInvalid:
            pass
        else:
            raise AssertionError("candidate binding tamper must fail")


def main() -> int:
    test_valid_and_tamper()
    test_external_and_candidate_binding()
    print("m8_final_evidence_profile_receipt_v2_tests=PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
