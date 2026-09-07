#!/usr/bin/env python3
from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "scripts" / "m8_profile_case_provenance_verifier.py"
SPEC = importlib.util.spec_from_file_location("m8_profile_case_provenance_verifier", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
verifier = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(verifier)


class TestFailure(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise TestFailure(message)


def sha(raw: bytes) -> str:
    return hashlib.sha256(raw).hexdigest()


def write_json(path: Path, value: object) -> bytes:
    raw = (json.dumps(value, indent=2, sort_keys=True) + "\n").encode("utf-8")
    path.write_bytes(raw)
    return raw


def raw_host() -> dict[str, object]:
    machine = {
        "schema_version": 1,
        "captured_at_utc": "2026-09-07T12:00:00Z",
        "device_class": "legacy-phone",
        "physical_device_confirmed": True,
        "physical_ram_mib": 2048,
        "logical_cpu_count": 4,
        "os_name": "Linux",
        "os_release": "test-kernel",
        "architecture": "x86_64",
        "cpu_model": "test-cpu",
        "run_label": "m8-profile-verifier-test",
        "thermal": {"state": "nominal", "source": "test", "readings_c": [40.0]},
    }
    return {
        "system_fingerprint_schema": "zevryon.competitor.system-fingerprint.v2",
        "machine_metadata_schema": 1,
        "platform": "Linux",
        "arch": "x86_64",
        "kernel": "test-kernel",
        "logical_cpus": 4,
        "cpu_model": "test-cpu",
        "physical_ram_mib": 2048,
        "device_class": "legacy-phone",
        "benchmark_machine_metadata": machine,
    }


def build_fixture(root: Path) -> dict[str, Path]:
    commit = "a" * 40
    tree = "b" * 40
    device = "legacy-phone"
    titan_path = root / "titan.zmdoc"
    copy_path = root / "copy-output.txt"
    payload = b"hello M8\n"
    titan_path.write_bytes(payload)
    copy_path.write_bytes(payload)
    payload_sha = sha(payload)

    titan = {
        "schema": verifier.TITAN_SCHEMA,
        "authority": verifier.TITAN_AUTHORITY,
        "mode": "certification",
        "candidate_commit": commit,
        "candidate_tree": tree,
        "observed_envelope": {"logical_utf8_bytes": len(payload)},
        "generation": {
            "container_sha256": payload_sha,
            "payload_sha256": payload_sha,
            "physical_bytes": len(payload),
        },
        "certification_eligible": True,
        "gate_passed": True,
    }
    titan_report_path = root / "titan-report.json"
    titan_raw = write_json(titan_report_path, titan)
    titan_report_sha = sha(titan_raw)

    before_host = raw_host()
    after_host = json.loads(json.dumps(before_host))
    before_cert = verifier.certify_physical_host(before_host, label="fixture-before")
    after_cert = verifier.certify_physical_host(after_host, label="fixture-after")
    fingerprint = verifier.normalized_system_fingerprint(before_host)
    host = {
        "schema": verifier.HOST_SCHEMA,
        "authority": before_cert["authority"],
        "device_class": device,
        "system_fingerprint": fingerprint,
        "before": {"host": before_host, "certification": before_cert},
        "after": {"host": after_host, "certification": after_cert},
        "same_system_fingerprint": True,
        "physical_host_gate_passed": True,
    }
    host_path = root / "physical-host.json"
    host_raw = write_json(host_path, host)
    host_sha = sha(host_raw)

    probe_path = root / "probe.bin"
    probe_path.write_bytes(b"probe-binary")
    probe_sha = sha(probe_path.read_bytes())

    identity = {
        "candidate_commit": commit,
        "candidate_tree": tree,
        "device_class": device,
        "titan_report_sha256": titan_report_sha,
        "container_sha256": payload_sha,
        "payload_sha256": payload_sha,
        "physical_host_receipt_sha256": host_sha,
    }
    case_id = verifier.canonical_sha256(identity)
    policy = {
        "profile": device,
        "within_profile_budgets": True,
        "hot_budget_bytes": 6_000_000,
        "hot_allocated_bytes": 1_000_000,
        "warm_budget_bytes": 6_000_000,
        "warm_allocated_bytes": 1_000_000,
        "cold_budget_bytes": 4_000_000,
        "cold_allocated_bytes": 1_000_000,
    }
    scroll = [float(index) for index in range(257)]
    mutation = [100.0 + index for index in range(257)]
    pss_samples_mb = [1.024, 2.048]
    case = {
        "schema": verifier.CASE_SCHEMA,
        "authority": verifier.CASE_AUTHORITY,
        "case_id": case_id,
        "candidate_commit": commit,
        "candidate_tree": tree,
        "device_class": device,
        "titan": {
            "report_sha256": titan_report_sha,
            "container_sha256": payload_sha,
            "payload_sha256": payload_sha,
        },
        "physical_host": {
            "authority": before_cert["authority"],
            "receipt_sha256": host_sha,
            "qualified": True,
            "device_class": device,
            "physical_memory_mib": 2048,
            "process_group_complete": True,
            "pss_authority": "aggregate-pss",
        },
        "runtime_policy": policy,
        "raw": {
            "pss_samples_mb": pss_samples_mb,
            "first_viewport_streaming_ms": 100.0,
            "first_viewport_preindexed_ms": 200.0,
            "scroll_samples_ms": scroll,
            "exact_search_cold_ms": 10.0,
            "exact_search_warm_ms": 2.0,
            "mutation_samples_us": mutation,
            "copy": {
                "source_bytes": len(payload),
                "output_bytes": len(payload),
                "elapsed_seconds": 1.0,
                "output_sha256": payload_sha,
                "output_valid_utf8": True,
                "cancelled": False,
            },
            "correctness": {"data_loss_events": 0, "invalid_utf8_events": 0, "crashes_or_ooms": 0},
            "probe_return_code": 0,
            "probe_terminated_abnormally": False,
        },
    }
    case_path = root / "profile-case.json"
    case_raw = write_json(case_path, case)

    phase_policy = dict(policy)
    phase_policy["layout_store_read_policy_applied"] = True
    phase = {
        "schema": "zevryon.m8.profile-probe.v1",
        "device_class": device,
        "runtime_policy": phase_policy,
        "streaming": {"milliseconds": 100.0},
        "preindexed": {"milliseconds": 200.0},
        "scroll": {"samples_ms": scroll},
        "search": {"cold_ms": 10.0, "warm_ms": 2.0},
        "mutation": {"samples_us": mutation},
        "copy": {"source_bytes": len(payload), "output_bytes": len(payload), "elapsed_seconds": 1.0, "cancelled": False},
    }
    pss = {
        "authority": verifier.PSS_AUTHORITY,
        "sampling_interval_seconds": 0.05,
        "root_pid": 123,
        "observed_pids": [123],
        "expected_single_process": True,
        "process_group_complete": True,
        "samples": [
            {"monotonic_ns": 100, "elapsed_ms": 0.0, "pids": [123], "per_pid_pss_kib": {"123": 1000}, "aggregate_pss_mb": 1.024},
            {"monotonic_ns": 200, "elapsed_ms": 50.0, "pids": [123], "per_pid_pss_kib": {"123": 2000}, "aggregate_pss_mb": 2.048},
        ],
    }
    provenance = {
        "schema": verifier.PROVENANCE_SCHEMA,
        "authority": verifier.PROVENANCE_AUTHORITY,
        "case_id": case_id,
        "candidate_commit": commit,
        "candidate_tree": tree,
        "device_class": device,
        "probe": {"path": str(probe_path), "sha256": probe_sha, "return_code": 0, "stderr": "", "stdout_sha256": "d" * 64},
        "titan": {"report_path": str(titan_report_path), "report_sha256": titan_report_sha, "corpus_path": str(titan_path), "container_sha256": payload_sha, "payload_sha256": payload_sha},
        "physical_host": {"path": str(host_path), "sha256": host_sha, "system_fingerprint": fingerprint},
        "process_group_pss": pss,
        "phase_receipts": phase,
        "copy_output": {"path": str(copy_path), "sha256": payload_sha, "bytes": len(payload), "valid_utf8": True},
        "case": {"path": str(case_path), "sha256": sha(case_raw)},
        "collector_gate_passed": True,
    }
    provenance_path = root / "profile-case-provenance.json"
    write_json(provenance_path, provenance)
    return {
        "provenance": provenance_path,
        "case": case_path,
        "host": host_path,
        "titan_report": titan_report_path,
        "titan": titan_path,
        "copy": copy_path,
        "probe": probe_path,
    }


def verify(paths: dict[str, Path]) -> dict[str, object]:
    return verifier.verify(
        provenance_path=paths["provenance"],
        case_path=paths["case"],
        host_path=paths["host"],
        titan_report_path=paths["titan_report"],
        titan_path=paths["titan"],
        copy_output_path=paths["copy"],
        probe_path=paths["probe"],
    )


def require_invalid(callback, message: str) -> None:
    try:
        callback()
    except verifier.EvidenceInvalid:
        return
    raise TestFailure(message)


def mutate_json(path: Path, callback) -> None:
    value = json.loads(path.read_text(encoding="utf-8"))
    callback(value)
    write_json(path, value)


def main() -> int:
    try:
        with tempfile.TemporaryDirectory() as raw:
            paths = build_fixture(Path(raw))
            result = verify(paths)
            require(result["provenance_gate_passed"] is True, "valid provenance did not pass")
            require(result["physical_host"]["before_and_after_recertified"] is True, "physical host was not independently recertified")
            require(result["phases"]["layout_store_read_policy_applied"] is True, "layout StoreReadConfig receipt was not enforced")
            require(result["pss"]["sample_count"] == 2, "PSS sample count drifted")

        with tempfile.TemporaryDirectory() as raw:
            paths = build_fixture(Path(raw))
            host = json.loads(paths["host"].read_text(encoding="utf-8"))
            host["before"]["host"]["benchmark_machine_metadata"]["physical_device_confirmed"] = False
            require_invalid(lambda: verifier.validate_physical_host_receipt(host, "legacy-phone"), "unconfirmed physical host was accepted")

        with tempfile.TemporaryDirectory() as raw:
            paths = build_fixture(Path(raw))
            mutate_json(paths["provenance"], lambda value: value["phase_receipts"]["runtime_policy"].__setitem__("layout_store_read_policy_applied", False))
            require_invalid(lambda: verify(paths), "missing layout StoreReadConfig binding was accepted")

        with tempfile.TemporaryDirectory() as raw:
            paths = build_fixture(Path(raw))
            mutate_json(paths["provenance"], lambda value: value["process_group_pss"]["samples"][0].__setitem__("aggregate_pss_mb", 99.0))
            require_invalid(lambda: verify(paths), "forged aggregate PSS was accepted")

        with tempfile.TemporaryDirectory() as raw:
            paths = build_fixture(Path(raw))
            paths["copy"].write_bytes(b"tampered")
            require_invalid(lambda: verify(paths), "tampered copy output was accepted")

        with tempfile.TemporaryDirectory() as raw:
            paths = build_fixture(Path(raw))
            mutate_json(paths["case"], lambda value: value.__setitem__("case_id", "0" * 64))
            provenance = json.loads(paths["provenance"].read_text(encoding="utf-8"))
            provenance["case"]["sha256"] = verifier.sha256_file(paths["case"])
            provenance["case_id"] = "0" * 64
            write_json(paths["provenance"], provenance)
            require_invalid(lambda: verify(paths), "forged case identity was accepted")

        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            receipt = root / "verification.json"
            verifier.write_exclusive(receipt, {"ok": True})
            require_invalid(lambda: verifier.write_exclusive(receipt, {"ok": False}), "verification receipt overwrite was accepted")
    except (TestFailure, OSError, ValueError, verifier.EvidenceInvalid) as exc:
        print(f"FAIL: {exc}")
        return 1
    print("m8-profile-case-provenance-verifier-tests: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
