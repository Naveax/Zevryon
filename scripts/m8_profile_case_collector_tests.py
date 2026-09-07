#!/usr/bin/env python3
from __future__ import annotations

import copy
import importlib.util
import json
import os
from pathlib import Path
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "scripts" / "m8_profile_case_collector.py"
SPEC = importlib.util.spec_from_file_location("m8_profile_case_collector", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
collector = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(collector)


class TestFailure(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise TestFailure(message)


def require_invalid(callback, message: str) -> None:
    try:
        callback()
    except collector.ProfileCaseInvalid:
        return
    raise TestFailure(message)


def titan() -> dict[str, object]:
    return {
        "schema": collector.TITAN_SCHEMA,
        "authority": collector.TITAN_AUTHORITY,
        "mode": "certification",
        "candidate_commit": "a" * 40,
        "candidate_tree": "b" * 40,
        "observed_envelope": {
            "logical_utf8_bytes": 4 * 1024 * 1024 * 1024,
            "logical_records": 8_388_608,
            "logical_nodes": 67_108_864,
            "style_runs": 33_554_432,
            "resource_references": 1_048_576,
            "largest_record_bytes": 64 * 1024 * 1024,
            "largest_unbroken_token_bytes": 16 * 1024 * 1024,
            "pathological_grapheme_bytes": 64 * 1024,
        },
        "generation": {
            "container_sha256": "c" * 64,
            "payload_sha256": "d" * 64,
            "physical_bytes": 1,
        },
        "certification_threshold_met": True,
        "certification_eligible": True,
        "gate_passed": True,
    }


def probe_document() -> dict[str, object]:
    envelope = titan()["observed_envelope"]
    assert isinstance(envelope, dict)
    return {
        "schema": collector.PROBE_SCHEMA,
        "device_class": "legacy-phone",
        "runtime_policy": {
            "profile": "legacy-phone",
            "within_profile_budgets": True,
            "hot_budget_bytes": 6_000_000,
            "hot_allocated_bytes": 6_000_000,
            "warm_budget_bytes": 6_000_000,
            "warm_allocated_bytes": 1_786_432,
            "cold_budget_bytes": 4_000_000,
            "cold_allocated_bytes": 4_000_000,
        },
        "store": {
            "logical_utf8_bytes": envelope["logical_utf8_bytes"],
            "logical_records": envelope["logical_records"],
            "logical_nodes": envelope["logical_nodes"],
            "style_runs": envelope["style_runs"],
            "resource_references": envelope["resource_references"],
            "largest_record_bytes": envelope["largest_record_bytes"],
            "payload_sha256": "d" * 64,
        },
        "streaming": {
            "milliseconds": 100.0,
            "preview_records": 1,
            "remaining_records": envelope["logical_records"] - 1,
            "fragment_count": 1,
            "truncated": False,
        },
        "preindexed": {
            "milliseconds": 200.0,
            "fragment_count": 1,
            "total_height_q8": 1000,
            "truncated": False,
        },
        "scroll": {
            "warmup_count": 16,
            "measured_count": 257,
            "coordinates_q8": list(range(257)),
            "samples_ms": [1.0 + index / 1000.0 for index in range(257)],
        },
        "search": {
            "query": "ZEVRYON_M8_TITAN_TAIL",
            "cold_ms": 10.0,
            "warm_ms": 2.0,
            "terminal_record_index": envelope["logical_records"] - 1,
            "terminal_logical_id": envelope["logical_records"] - 1,
            "terminal_byte_offset": 0,
            "hit_count": 1,
            "cache_before_cold_misses": 0,
            "cache_after_cold_misses": 1,
            "cache_after_warm_hot_hits": 1,
            "cache_after_warm_warm_hits": 0,
        },
        "mutation": {
            "warmup_count": 16,
            "measured_count": 257,
            "indices": list(range(257)),
            "samples_us": [100.0 + index / 10.0 for index in range(257)],
            "restored": True,
            "initial_total_height_q8": 123456,
        },
        "copy": {
            "source_bytes": envelope["logical_utf8_bytes"],
            "output_bytes": envelope["logical_utf8_bytes"],
            "elapsed_seconds": 10.0,
            "records_exported": envelope["logical_records"],
            "peak_input_block_bytes": 4096,
            "peak_output_buffer_bytes": 4096,
            "peak_validation_codepoints": 0,
            "cancelled": False,
        },
    }


def test_probe_validation() -> None:
    document = probe_document()
    validated = collector.validate_probe_document(
        document,
        collector.DeviceClass.LEGACY_PHONE,
        titan(),
    )
    require(validated["device_class"] == "legacy-phone", "valid probe was not accepted")

    bad = copy.deepcopy(document)
    bad["streaming"]["remaining_records"] = 0
    require_invalid(
        lambda: collector.validate_probe_document(
            bad,
            collector.DeviceClass.LEGACY_PHONE,
            titan(),
        ),
        "post-import streaming viewport was accepted",
    )

    bad = copy.deepcopy(document)
    bad["scroll"]["samples_ms"] = [1.0] * 256
    require_invalid(
        lambda: collector.validate_probe_document(
            bad,
            collector.DeviceClass.LEGACY_PHONE,
            titan(),
        ),
        "short scroll sample set was accepted",
    )


def test_case_derivation() -> None:
    pss = [
        {
            "monotonic_ns": 1,
            "elapsed_ms": 0.0,
            "pids": [123],
            "per_pid_pss_kib": {"123": 1000},
            "aggregate_pss_mb": 1.024,
        },
        {
            "monotonic_ns": 2,
            "elapsed_ms": 50.0,
            "pids": [123],
            "per_pid_pss_kib": {"123": 2000},
            "aggregate_pss_mb": 2.048,
        },
    ]
    case = collector.build_case_document(
        profile=collector.DeviceClass.LEGACY_PHONE,
        candidate_commit="a" * 40,
        candidate_tree="b" * 40,
        titan_report=titan(),
        titan_report_sha256="e" * 64,
        host_receipt_sha256="f" * 64,
        physical_ram_mib=2048,
        pss_samples=pss,
        probe_document=probe_document(),
        copy_sha256="d" * 64,
        copy_valid_utf8=True,
    )
    require(case["schema"] == collector.CASE_SCHEMA, "case schema drifted")
    require(case["authority"] == collector.CASE_AUTHORITY, "case authority drifted")
    raw = case["raw"]
    require(raw["pss_samples_mb"] == [1.024, 2.048], "PSS samples were not preserved")
    require(raw["correctness"] == {
        "data_loss_events": 0,
        "invalid_utf8_events": 0,
        "crashes_or_ooms": 0,
    }, "correctness axes were not derived from raw receipts")

    loss = collector.build_case_document(
        profile=collector.DeviceClass.LEGACY_PHONE,
        candidate_commit="a" * 40,
        candidate_tree="b" * 40,
        titan_report=titan(),
        titan_report_sha256="e" * 64,
        host_receipt_sha256="f" * 64,
        physical_ram_mib=2048,
        pss_samples=pss,
        probe_document=probe_document(),
        copy_sha256="0" * 64,
        copy_valid_utf8=False,
    )
    require(loss["raw"]["correctness"]["data_loss_events"] == 1, "copy hash mismatch did not force data loss")
    require(loss["raw"]["correctness"]["invalid_utf8_events"] == 1, "invalid UTF-8 did not force the UTF-8 axis")


def test_utf8_hashing() -> None:
    with tempfile.TemporaryDirectory() as raw:
        root = Path(raw)
        valid = root / "valid.txt"
        valid.write_bytes("héllo\nالعربية\n".encode("utf-8"))
        digest, ok, size = collector.hash_and_validate_utf8(valid)
        require(ok, "valid UTF-8 was rejected")
        require(digest == collector.sha256_file(valid), "combined UTF-8/hash pass drifted")
        require(size == valid.stat().st_size, "combined UTF-8/hash byte count drifted")

        invalid = root / "invalid.txt"
        invalid.write_bytes(b"good\xffbad")
        _, ok, _ = collector.hash_and_validate_utf8(invalid)
        require(not ok, "invalid UTF-8 was accepted")


def test_proc_stat_parser() -> None:
    with tempfile.TemporaryDirectory() as raw:
        path = Path(raw) / "stat"
        path.write_text("123 (worker name) R 10 456 456 0 0 0\n", encoding="ascii")
        require(collector._process_group_id_from_stat(path) == 456, "proc stat pgrp parser drifted")


def test_override_rejection() -> None:
    previous_ram = os.environ.get("ZEVRYON_PHYSICAL_RAM_MIB")
    previous_profile = os.environ.get("ZEVRYON_DEVICE_PROFILE")
    try:
        os.environ["ZEVRYON_PHYSICAL_RAM_MIB"] = "2048"
        require_invalid(
            collector.validate_no_physical_identity_overrides,
            "physical RAM override was accepted for certification",
        )
        del os.environ["ZEVRYON_PHYSICAL_RAM_MIB"]
        os.environ["ZEVRYON_DEVICE_PROFILE"] = "legacy-phone"
        require_invalid(
            collector.validate_no_physical_identity_overrides,
            "device profile override was accepted for certification",
        )
    finally:
        if previous_ram is None:
            os.environ.pop("ZEVRYON_PHYSICAL_RAM_MIB", None)
        else:
            os.environ["ZEVRYON_PHYSICAL_RAM_MIB"] = previous_ram
        if previous_profile is None:
            os.environ.pop("ZEVRYON_DEVICE_PROFILE", None)
        else:
            os.environ["ZEVRYON_DEVICE_PROFILE"] = previous_profile


def main() -> int:
    try:
        test_probe_validation()
        test_case_derivation()
        test_utf8_hashing()
        test_proc_stat_parser()
        test_override_rejection()
    except (TestFailure, OSError, ValueError, collector.ProfileCaseInvalid) as exc:
        print(f"FAIL: {exc}")
        return 1
    print("m8-profile-case-collector-tests: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
