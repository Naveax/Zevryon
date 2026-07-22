from __future__ import annotations

import copy

import pytest

from scripts.zenith_benchmark_contract import (
    BenchmarkContractError,
    build_giant_record_report,
    validate_report,
)


def input_reports() -> tuple[dict, dict, dict]:
    massivedoc = {
        "logical_bytes": 134_217_728,
        "logical_records": 131_072,
        "giant_record_bytes": 67_108_864,
        "payload_sha256": "a" * 64,
        "zero_data_loss": True,
    }
    global_layout = {
        "checkpoint_accelerated": True,
        "bounded_global_random_access": True,
        "measurement": {
            "seconds": 0.003,
            "peak_pss_bytes": 400_000,
            "result": {"layout": {"source_bytes_read": 131_072}},
        },
    }
    hot_scroll = {
        "hot_scroll": {
            "peak_pss_bytes": 2_300_000,
            "result": {
                "queries": 257,
                "checkpoint_cache_budget_bytes": 1_000_000,
                "source_window_cache_budget_bytes": 524_288,
                "random": {
                    "p50_ms": 0.19,
                    "p95_ms": 0.22,
                    "p99_ms": 0.28,
                    "maximum_ms": 0.37,
                    "total_source_bytes_read": 16_842_752,
                },
                "adjacent": {
                    "p50_ms": 0.15,
                    "p95_ms": 0.19,
                    "p99_ms": 0.23,
                    "maximum_ms": 0.24,
                    "total_source_bytes_read": 327_680,
                },
                "random_session": {
                    "checkpoint_cache_bytes": 98_528,
                    "checkpoint_cache_peak_bytes": 98_528,
                    "checkpoint_cache_hits": 515,
                    "checkpoint_cache_misses": 1,
                    "checkpoint_cache_evictions": 0,
                    "source_window_cache_bytes": 459_816,
                    "source_window_cache_peak_bytes": 459_816,
                    "source_window_cache_hits": 0,
                    "source_window_cache_misses": 258,
                    "source_window_cache_evictions": 250,
                },
                "adjacent_session": {
                    "checkpoint_cache_bytes": 98_528,
                    "checkpoint_cache_peak_bytes": 98_528,
                    "checkpoint_cache_hits": 515,
                    "checkpoint_cache_misses": 1,
                    "checkpoint_cache_evictions": 0,
                    "source_window_cache_bytes": 394_128,
                    "source_window_cache_peak_bytes": 394_128,
                    "source_window_cache_hits": 252,
                    "source_window_cache_misses": 6,
                    "source_window_cache_evictions": 0,
                },
            },
        },
        "verify_after_hot_scroll": {"result": {"ok": True}},
        "zero_payload_data_loss": True,
        "bounded_checkpoint_cache": True,
        "bounded_source_window_cache": True,
        "random_p95_gate_ms": 2.0,
        "adjacent_p95_gate_ms": 0.5,
        "maximum_source_bytes_gate": 65_536,
    }
    return massivedoc, global_layout, hot_scroll


def build_report() -> dict:
    massivedoc, global_layout, hot_scroll = input_reports()
    return build_giant_record_report(
        massivedoc=massivedoc,
        global_layout=global_layout,
        hot_scroll=hot_scroll,
        commit_sha="1" * 40,
        compiler="clang++",
        build_type="Release",
        runner_os="Linux",
    )


def test_unified_report_is_valid() -> None:
    report = build_report()
    validate_report(report)
    assert report["schema"] == "zevryon.benchmark.v1"
    assert [stage["mode"] for stage in report["stages"]] == ["warm", "hot", "hot"]
    assert report["resources"]["source_window"]["peak_bytes"] == 459_816
    assert all(report["correctness"].values())


def test_unknown_commit_is_rejected() -> None:
    report = build_report()
    report["build"]["commit_sha"] = "unknown"
    with pytest.raises(BenchmarkContractError, match="commit SHA required"):
        validate_report(report)


def test_non_monotonic_percentiles_are_rejected() -> None:
    report = build_report()
    report["stages"][1]["latency_ms"]["p95"] = 0.10
    with pytest.raises(BenchmarkContractError, match="not monotonic"):
        validate_report(report)


def test_resource_peak_cannot_exceed_hard_limit() -> None:
    report = build_report()
    report["resources"]["source_window"]["peak_bytes"] = 524_289
    with pytest.raises(BenchmarkContractError, match="peak bytes exceed hard limit"):
        validate_report(report)


def test_failed_correctness_gate_is_rejected() -> None:
    report = copy.deepcopy(build_report())
    report["correctness"]["post_run_payload_verification"] = False
    with pytest.raises(BenchmarkContractError, match="correctness gates failed"):
        validate_report(report)
