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

SCRIPT = SOURCE_ROOT / "scripts" / "m8_profile_collection_binder.py"
COMMIT = "a" * 40
TREE = "b" * 40
CONTAINER_SHA = "c" * 64
PAYLOAD_SHA = "d" * 64
HOST_SHA = "e" * 64
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


def run_binder(root: Path, titan: dict, cases: list[dict], suffix: str) -> tuple[subprocess.CompletedProcess[str], Path, Path]:
    titan_path = root / f"titan-{suffix}.json"
    write_json(titan_path, titan)
    case_paths: list[Path] = []
    for index, case in enumerate(cases):
        path = root / f"case-{suffix}-{index}.json"
        write_json(path, case)
        case_paths.append(path)
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
    for path in case_paths:
        command.extend(["--case", str(path)])
    result = subprocess.run(command, text=True, capture_output=True, check=False)
    return result, observations, receipt


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="zevryon-m8-profile-binder-") as temporary:
        root = Path(temporary)

        titan = titan_report()
        titan_path = root / "titan-seed.json"
        write_json(titan_path, titan)
        titan_sha = sha256(titan_path)
        base_cases = [profile_case(device, titan_sha, index) for index, device in enumerate(DeviceClass)]

        happy, observations_path, receipt_path = run_binder(root, titan, copy.deepcopy(base_cases), "happy")
        require(happy.returncode == 0, f"happy binder failed:\nstdout={happy.stdout}\nstderr={happy.stderr}")
        observations = json.loads(observations_path.read_text(encoding="utf-8"))
        receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
        require(len(observations["observations"]) == 4, "happy binder did not emit four observations")
        require(receipt["recomputed_four_profile_gate_passed"] is True, "happy binder gate did not pass")
        require(receipt["observations_sha256"] == sha256(observations_path), "observation SHA receipt drifted")

        duplicate = copy.deepcopy(base_cases)
        duplicate[-1]["device_class"] = duplicate[0]["device_class"]
        duplicate[-1]["physical_host"]["device_class"] = duplicate[0]["device_class"]
        duplicate[-1]["runtime_policy"]["profile"] = duplicate[0]["device_class"]
        bad, _, _ = run_binder(root, titan, duplicate, "duplicate")
        require(bad.returncode == 1, "duplicate profile set was not evidence-invalid")

        low_ram = copy.deepcopy(base_cases)
        low_ram[0]["physical_host"]["physical_memory_mib"] = DEVICE_PROFILES[DeviceClass.LEGACY_PHONE].minimum_physical_ram_mib - 1
        bad, _, _ = run_binder(root, titan, low_ram, "low-ram")
        require(bad.returncode == 1, "under-qualified physical host was not rejected")

        mixed_candidate = copy.deepcopy(base_cases)
        mixed_candidate[1]["candidate_tree"] = "f" * 40
        bad, _, _ = run_binder(root, titan, mixed_candidate, "mixed-candidate")
        require(bad.returncode == 1, "mixed candidate evidence was not rejected")

        over_budget = copy.deepcopy(base_cases)
        over_budget[2]["runtime_policy"]["hot_allocated_bytes"] = over_budget[2]["runtime_policy"]["hot_budget_bytes"] + 1
        bad, _, _ = run_binder(root, titan, over_budget, "over-budget")
        require(bad.returncode == 1, "over-budget runtime policy was not rejected")

        hand_authored = copy.deepcopy(base_cases)
        hand_authored[0]["gate_passed"] = True
        bad, _, _ = run_binder(root, titan, hand_authored, "hand-authored")
        require(bad.returncode == 1, "hand-authored verdict field was not rejected")

        slow_scroll = copy.deepcopy(base_cases)
        desktop_profile = DEVICE_PROFILES[DeviceClass.DESKTOP]
        desktop_index = list(DeviceClass).index(DeviceClass.DESKTOP)
        slow_scroll[desktop_index]["raw"]["scroll_samples_ms"] = [float(desktop_profile.scroll_p99_ms) * 2.0] * 257
        valid_fail, _, fail_receipt_path = run_binder(root, titan, slow_scroll, "slow-scroll")
        require(valid_fail.returncode == 2, "valid performance miss did not return exit 2")
        fail_receipt = json.loads(fail_receipt_path.read_text(encoding="utf-8"))
        require(fail_receipt["recomputed_four_profile_gate_passed"] is False, "valid performance miss was reported as passing")

        copy_loss = copy.deepcopy(base_cases)
        copy_loss[0]["raw"]["copy"]["output_sha256"] = "0" * 64
        valid_fail, copy_obs_path, _ = run_binder(root, titan, copy_loss, "copy-loss")
        require(valid_fail.returncode == 2, "copy hash mismatch did not become valid failing evidence")
        copy_obs = json.loads(copy_obs_path.read_text(encoding="utf-8"))
        legacy = next(item for item in copy_obs["observations"] if item["device_class"] == DeviceClass.LEGACY_PHONE.value)
        require(legacy["data_loss_events"] >= 1, "copy hash mismatch did not increment data-loss evidence")

    print("m8-profile-collection-binder-tests: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
