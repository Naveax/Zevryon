#!/usr/bin/env python3
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import tempfile

import m8_bundle_import_profile as subject
from m8_bundle_common import ARTIFACT_PATHS, RECEIPT_PATHS, EvidenceInvalid
from zevryon_platform.performance_contract import DeviceClass

COMMIT, TREE, SOURCES = "1" * 40, "2" * 40, {"test": "3" * 64}


def jbytes(value: object) -> bytes:
    return (json.dumps(value, indent=2, sort_keys=True) + "\n").encode()


def plan() -> dict[str, object]:
    return {
        "bundle_id": "4" * 32,
        "candidate_commit": COMMIT,
        "candidate_tree": TREE,
        "artifacts": dict(ARTIFACT_PATHS),
        "receipts": dict(RECEIPT_PATHS),
        "authority_source_sha256": dict(SOURCES),
    }


def install_mocks(value: dict[str, object]) -> None:
    subject.load_bundle_plan = lambda root, verify_current=True: (value, b"plan")
    subject.clean_git_identity = lambda: (COMMIT, TREE)
    subject.authority_source_hashes = lambda: dict(SOURCES)


def attempts(root: Path) -> dict[DeviceClass, Path]:
    result = {}
    for index, device in enumerate(DeviceClass):
        directory = root / device.value
        (directory / "probe-work").mkdir(parents=True)
        (directory / "profile-case.json").write_text(json.dumps({"device_class": device.value, "case_id": f"case-{index}"}) + "\n")
        (directory / "profile-case-provenance.json").write_text("{}\n")
        (directory / "physical-host.json").write_text("{}\n")
        (directory / "probe-work" / "copy-output.txt").write_bytes(f"copy-{index}".encode())
        result[device] = directory
    return result


def externals(root: Path) -> tuple[Path, Path, Path]:
    report, titan, probe = root / "titan.json", root / "titan.zmdoc", root / "probe"
    report.write_text("{}\n")
    titan.write_bytes(b"titan")
    probe.write_bytes(b"probe")
    return report, titan, probe


def verifier(**kwargs):
    case = json.loads(Path(kwargs["case_path"]).read_text())
    return {
        "schema": "zevryon.m8.profile-case-provenance-verification.v1",
        "authority": "m8-profile-case-provenance-verifier-v1",
        "candidate_commit": COMMIT,
        "candidate_tree": TREE,
        "device_class": case["device_class"],
        "case_id": case["case_id"],
        "provenance_gate_passed": True,
    }


def binder(passed: bool):
    def run(*, titan_path, case_paths, verification_paths):
        assert set(case_paths) == set(DeviceClass) == set(verification_paths)
        obs = {"candidate_commit": COMMIT, "candidate_tree": TREE, "profiles": [d.value for d in DeviceClass]}
        receipt = {
            "schema": "zevryon.m8.profile-collection.v2",
            "authority": "m8-four-profile-collection-binder-v2",
            "candidate_commit": COMMIT,
            "candidate_tree": TREE,
            "all_provenance_receipts_verified": True,
            "verified_provenance_receipt_count": 4,
            "observations_sha256": hashlib.sha256(jbytes(obs)).hexdigest(),
        }
        return obs, receipt, passed
    return run


def run_case(passed: bool) -> tuple[int, Path, dict[str, object]]:
    temp = tempfile.TemporaryDirectory()
    root = Path(temp.name)
    bundle = root / "bundle"
    bundle.mkdir()
    install_mocks(plan())
    report, titan, probe = externals(root)
    code, receipt = subject.perform_import(
        artifact_root=bundle,
        titan_report=report,
        titan=titan,
        probe=probe,
        attempts=attempts(root),
        verifier=verifier,
        binder=binder(passed),
    )
    receipt["_temp"] = temp
    return code, bundle, receipt


def test_success_create_only_and_gate_failure() -> None:
    code, bundle, receipt = run_case(True)
    assert code == 0 and receipt["evidence_valid"] is True and receipt["recomputed_gate_passed"] is True
    assert (bundle / ARTIFACT_PATHS["profile"]).is_file()
    for relative in subject.PROFILE_SUPPORT_PATHS.values():
        assert (bundle / relative).is_file()
    temp = receipt.pop("_temp")
    temp.cleanup()

    code, bundle, receipt = run_case(False)
    assert code == 2 and receipt["evidence_valid"] is True and receipt["recomputed_gate_passed"] is False
    assert (bundle / ARTIFACT_PATHS["profile"]).is_file()
    temp = receipt.pop("_temp")
    temp.cleanup()


def test_identity_tamper_rejected() -> None:
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        bundle_root = root / "bundle"
        bundle_root.mkdir()
        install_mocks(plan())
        report, titan, probe = externals(root)
        attempt_set = attempts(root)

        def bad(**kwargs):
            value = verifier(**kwargs)
            value["candidate_commit"] = "f" * 40
            return value

        try:
            subject.perform_import(
                artifact_root=bundle_root,
                titan_report=report,
                titan=titan,
                probe=probe,
                attempts=attempt_set,
                verifier=bad,
                binder=binder(True),
            )
        except EvidenceInvalid:
            pass
        else:
            raise AssertionError("tampered verifier identity must fail")
        assert not (bundle_root / ARTIFACT_PATHS["profile"]).exists()


def test_attempt_binding_rules() -> None:
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        values = attempts(root)
        complete = [f"{device.value}={values[device]}" for device in DeviceClass]
        assert set(subject.parse_attempts(complete)) == set(DeviceClass)
        for bad in (complete[:-1], [complete[0], complete[0], complete[1], complete[2]]):
            try:
                subject.parse_attempts(bad)
            except EvidenceInvalid:
                pass
            else:
                raise AssertionError("invalid attempt binding must fail")


def main() -> int:
    test_success_create_only_and_gate_failure()
    test_identity_tamper_rejected()
    test_attempt_binding_rules()
    print("m8_bundle_import_profile_tests=PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
