#!/usr/bin/env python3
from __future__ import annotations

import copy
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

SOURCE_ROOT = Path(__file__).resolve().parents[1]
if str(SOURCE_ROOT) not in sys.path:
    sys.path.insert(0, str(SOURCE_ROOT))

from zevryon_platform.performance_contract import DEVICE_PROFILES, TITAN_WORST_CASE, DeviceClass  # noqa: E402

SCRIPT = SOURCE_ROOT / "scripts" / "m8_profile_collection_binder_v2.py"
COMMIT = "a" * 40
TREE = "b" * 40
CONTAINER_SHA = "c" * 64
PAYLOAD_SHA = "d" * 64
HOST_SHA = "e" * 64
PROVENANCE_SHA = "f" * 64
PROBE_SHA = "1" * 64
MIB = 1024 * 1024
DECIMAL_MB = 1_000_000


def write_json(path: Path, value: object) -> None:
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def titan_report() -> dict:
    envelope = dict(TITAN_WORST_CASE.__dict__)
    return {
        "schema": "zevryon.m8.titan-fixture.v1",
        "authority": "m8-canonical-titan-fixture-v1",
        "mode": "certification",
        "candidate_commit": COMMIT,
        "candidate_tree": TREE,
        "corpus_path": "/evidence/titan.zmdoc",
        "observed_envelope": envelope,
        "frozen_certification_envelope": envelope,
        "generation": {
            "container_sha256": CONTAINER_SHA,
            "payload_sha256": PAYLOAD_SHA,
            "physical_bytes": TITAN_WORST_CASE.logical_utf8_bytes + 1024,
        },
        "verification": {"synthetic_test_fixture": True},
        "certification_threshold_met": True,
        "certification_eligible": True,
        "gate_passed": True,
    }


def profile_case(device: DeviceClass, titan_sha: str, ordinal: int) -> dict:
    profile = DEVICE_PROFILES[device]
    throughput = float(profile.copy_throughput_mib_s) * 1.25
    copy_seconds = (TITAN_WORST_CASE.logical_utf8_bytes / MIB) / throughput
    return {
        "schema": "zevryon.m8.profile-case.v1",
        "authority": "m8-raw-profile-case-v1",
        "case_id": f"case-{ordinal}-{device.value}",
        "candidate_commit": COMMIT,
        "candidate_tree": TREE,
        "device_class": device.value,
        "titan": {
            "report_sha256": titan_sha,
            "container_sha256": CONTAINER_SHA,
            "payload_sha256": PAYLOAD_SHA,
        },
        "physical_host": {
            "authority": "synthetic-m8-profile-test-host",
            "receipt_sha256": HOST_SHA,
            "qualified": True,
            "device_class": device.value,
            "physical_memory_mib": profile.minimum_physical_ram_mib,
            "process_group_complete": True,
            "pss_authority": "aggregate-pss",
        },
        "runtime_policy": {
            "profile": device.value,
            "within_profile_budgets": True,
            "hot_budget_bytes": profile.hot_cache_mb * DECIMAL_MB,
            "hot_allocated_bytes": profile.hot_cache_mb * DECIMAL_MB // 2,
            "warm_budget_bytes": profile.warm_cache_mb * DECIMAL_MB,
            "warm_allocated_bytes": profile.warm_cache_mb * DECIMAL_MB // 2,
            "cold_budget_bytes": profile.cold_cache_mb * DECIMAL_MB,
            "cold_allocated_bytes": profile.cold_cache_mb * DECIMAL_MB // 2,
        },
        "raw": {
            "pss_samples_mb": [float(profile.process_group_pss_target_mb) * 0.75, float(profile.process_group_pss_target_mb) * 0.8],
            "first_viewport_streaming_ms": float(profile.first_viewport_streaming_ms) * 0.5,
            "first_viewport_preindexed_ms": float(profile.first_viewport_preindexed_ms) * 0.5,
            "scroll_samples_ms": [float(profile.scroll_p99_ms) * 0.5] * 257,
            "exact_search_cold_ms": float(profile.exact_search_cold_ms) * 0.5,
            "exact_search_warm_ms": float(profile.exact_search_warm_ms) * 0.5,
            "mutation_samples_us": [float(profile.mutation_p95_us) * 0.5] * 257,
            "copy": {
                "source_bytes": TITAN_WORST_CASE.logical_utf8_bytes,
                "output_bytes": TITAN_WORST_CASE.logical_utf8_bytes,
                "elapsed_seconds": copy_seconds,
                "output_sha256": PAYLOAD_SHA,
                "output_valid_utf8": True,
                "cancelled": False,
            },
            "correctness": {
                "data_loss_events": 0,
                "invalid_utf8_events": 0,
                "crashes_or_ooms": 0,
            },
            "probe_return_code": 0,
            "probe_terminated_abnormally": False,
        },
    }


def verification_receipt(device: DeviceClass, case_path: Path, titan_path: Path) -> dict:
    case = json.loads(case_path.read_text(encoding="utf-8"))
    profile = DEVICE_PROFILES[device]
    return {
        "schema": "zevryon.m8.profile-case-provenance-verification.v1",
        "authority": "m8-profile-case-provenance-verifier-v1",
        "candidate_commit": case["candidate_commit"],
        "candidate_tree": case["candidate_tree"],
        "device_class": device.value,
        "case_id": case["case_id"],
        "artifacts": {
            "provenance_sha256": PROVENANCE_SHA,
            "case_sha256": sha256(case_path),
            "physical_host_sha256": case["physical_host"]["receipt_sha256"],
            "titan_report_sha256": sha256(titan_path),
            "titan_container_sha256": CONTAINER_SHA,
            "copy_output_sha256": PAYLOAD_SHA,
            "probe_sha256": PROBE_SHA,
        },
        "physical_host": {
            "system_fingerprint": f"synthetic-{device.value}",
            "physical_ram_mib": profile.minimum_physical_ram_mib,
            "before_and_after_recertified": True,
        },
        "pss": {
            "sample_count": 2,
            "root_pid": 1234,
            "maximum_aggregate_pss_mb": max(case["raw"]["pss_samples_mb"]),
        },
        "phases": {
            "layout_store_read_policy_applied": True,
            "scroll_samples": 257,
            "mutation_samples": 257,
        },
        "provenance_gate_passed": True,
    }


def materialize(root: Path, titan: dict, cases: list[dict], suffix: str) -> tuple[Path, dict[DeviceClass, Path], dict[DeviceClass, Path]]:
    titan_path = root / f"titan-{suffix}.json"
    write_json(titan_path, titan)
    titan_sha = sha256(titan_path)

    case_paths: dict[DeviceClass, Path] = {}
    verification_paths: dict[DeviceClass, Path] = {}
    for index, device in enumerate(DeviceClass):
        case = copy.deepcopy(cases[index])
        case["titan"]["report_sha256"] = titan_sha
        case_path = root / f"case-{suffix}-{device.value}.json"
        write_json(case_path, case)
        verification_path = root / f"verify-{suffix}-{device.value}.json"
        write_json(verification_path, verification_receipt(device, case_path, titan_path))
        case_paths[device] = case_path
        verification_paths[device] = verification_path
    return titan_path, case_paths, verification_paths


def run_binder(
    root: Path,
    titan_path: Path,
    case_paths: dict[DeviceClass, Path],
    verification_paths: dict[DeviceClass, Path],
    suffix: str,
) -> tuple[subprocess.CompletedProcess[str], Path, Path]:
    observations = root / f"observations-{suffix}.json"
    receipt = root / f"receipt-{suffix}.json"
    command = [
        sys.executable,
        str(SCRIPT),
        "--titan-report",
        str(titan_path),
        "--observations-output",
        str(observations),
        "--receipt-output",
        str(receipt),
    ]
    for device in DeviceClass:
        if device in case_paths:
            command.extend(["--case", f"{device.value}={case_paths[device]}"])
        if device in verification_paths:
            command.extend(["--verification", f"{device.value}={verification_paths[device]}"])
    result = subprocess.run(command, text=True, capture_output=True, check=False)
    return result, observations, receipt


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def rewrite_verification(path: Path, mutate) -> None:
    value = json.loads(path.read_text(encoding="utf-8"))
    mutate(value)
    write_json(path, value)


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="zevryon-m8-profile-binder-v2-") as temporary:
        root = Path(temporary)
        titan = titan_report()
        seed_titan = root / "seed-titan.json"
        write_json(seed_titan, titan)
        seed_sha = sha256(seed_titan)
        base_cases = [profile_case(device, seed_sha, index) for index, device in enumerate(DeviceClass)]

        titan_path, cases, verifications = materialize(root, titan, base_cases, "happy")
        happy, observations_path, receipt_path = run_binder(root, titan_path, cases, verifications, "happy")
        require(happy.returncode == 0, f"happy v2 binder failed:\nstdout={happy.stdout}\nstderr={happy.stderr}")
        receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
        require(receipt["schema"] == "zevryon.m8.profile-collection.v2", "v2 collection schema missing")
        require(receipt["authority"] == "m8-four-profile-collection-binder-v2", "v2 collection authority missing")
        require(receipt["all_provenance_receipts_verified"] is True, "v2 provenance aggregate gate missing")
        require(receipt["verified_provenance_receipt_count"] == 4, "v2 verification receipt count drifted")
        require(len(receipt["verified_provenance_receipts"]) == 4, "v2 receipt list is incomplete")
        require(receipt["observations_sha256"] == sha256(observations_path), "v2 observation SHA receipt drifted")

        missing = dict(verifications)
        missing.pop(DeviceClass.DESKTOP)
        bad, _, _ = run_binder(root, titan_path, cases, missing, "missing-verification")
        require(bad.returncode == 1, "missing verification receipt was accepted")

        titan_path, cases, verifications = materialize(root, titan, base_cases, "bad-case-sha")
        target = verifications[DeviceClass.LEGACY_PHONE]
        rewrite_verification(target, lambda value: value["artifacts"].__setitem__("case_sha256", "0" * 64))
        bad, _, _ = run_binder(root, titan_path, cases, verifications, "bad-case-sha")
        require(bad.returncode == 1, "verification receipt with wrong case SHA was accepted")

        titan_path, cases, verifications = materialize(root, titan, base_cases, "bad-case-id")
        target = verifications[DeviceClass.MID_PHONE]
        rewrite_verification(target, lambda value: value.__setitem__("case_id", "wrong-case-id"))
        bad, _, _ = run_binder(root, titan_path, cases, verifications, "bad-case-id")
        require(bad.returncode == 1, "verification receipt with wrong case_id was accepted")

        titan_path, cases, verifications = materialize(root, titan, base_cases, "bad-tree")
        target = verifications[DeviceClass.MODERN_PHONE]
        rewrite_verification(target, lambda value: value.__setitem__("candidate_tree", "9" * 40))
        bad, _, _ = run_binder(root, titan_path, cases, verifications, "bad-tree")
        require(bad.returncode == 1, "verification receipt with wrong candidate tree was accepted")

        titan_path, cases, verifications = materialize(root, titan, base_cases, "failed-provenance")
        target = verifications[DeviceClass.DESKTOP]
        rewrite_verification(target, lambda value: value.__setitem__("provenance_gate_passed", False))
        bad, _, _ = run_binder(root, titan_path, cases, verifications, "failed-provenance")
        require(bad.returncode == 1, "failed provenance gate was accepted")

        titan_path, cases, verifications = materialize(root, titan, base_cases, "bad-titan-sha")
        target = verifications[DeviceClass.LEGACY_PHONE]
        rewrite_verification(target, lambda value: value["artifacts"].__setitem__("titan_report_sha256", "2" * 64))
        bad, _, _ = run_binder(root, titan_path, cases, verifications, "bad-titan-sha")
        require(bad.returncode == 1, "verification receipt with wrong Titan report SHA was accepted")

        titan_path, cases, verifications = materialize(root, titan, base_cases, "profile-swap")
        swapped = dict(cases)
        swapped[DeviceClass.LEGACY_PHONE], swapped[DeviceClass.MID_PHONE] = swapped[DeviceClass.MID_PHONE], swapped[DeviceClass.LEGACY_PHONE]
        bad, _, _ = run_binder(root, titan_path, swapped, verifications, "profile-swap")
        require(bad.returncode == 1, "profile-key/case mismatch was accepted")

        slow_cases = copy.deepcopy(base_cases)
        desktop_index = list(DeviceClass).index(DeviceClass.DESKTOP)
        desktop_profile = DEVICE_PROFILES[DeviceClass.DESKTOP]
        slow_cases[desktop_index]["raw"]["scroll_samples_ms"] = [float(desktop_profile.scroll_p99_ms) * 2.0] * 257
        titan_path, cases, verifications = materialize(root, titan, slow_cases, "valid-performance-miss")
        valid_fail, _, fail_receipt_path = run_binder(root, titan_path, cases, verifications, "valid-performance-miss")
        require(valid_fail.returncode == 2, "valid performance miss did not preserve no-compensation exit 2")
        fail_receipt = json.loads(fail_receipt_path.read_text(encoding="utf-8"))
        require(fail_receipt["recomputed_four_profile_gate_passed"] is False, "valid performance miss was reported as passing")
        require(fail_receipt["all_provenance_receipts_verified"] is True, "valid performance miss lost provenance verification")

    print("m8-profile-collection-binder-v2-tests: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
