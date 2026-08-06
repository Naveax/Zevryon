from __future__ import annotations

import copy
import hashlib
import json
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import z2r3d_finalize_unicode_promotion as finalize_mod
import z2r3d_unicode_certification as certify_mod


def canonical_sha(value: dict) -> str:
    return hashlib.sha256(
        json.dumps(value, sort_keys=True, separators=(",", ":")).encode("utf-8")
    ).hexdigest()


def sample_result(*, shadow: bool, healthy: bool = True, mismatches: int = 0) -> dict:
    return {
        "schema": "zevryon.z2r3du.unicode-workload.v1",
        "logical_bytes": 16 * 1024 * 1024,
        "rounds": 2,
        "feed_calls": 100,
        "finish_calls": 10,
        "reset_calls": 20,
        "decoded_records": 1000,
        "strict_failures": 7,
        "replacement_records": 10,
        "malformed_cases": 7,
        "discontinuity_cases": 1,
        "budget_cases": 1,
        "peak_output_records": 100,
        "semantic_checksum": "0123456789abcdef",
        "elapsed_ms": 100.0,
        "shadow": {
            "enabled": shadow,
            "healthy": healthy if shadow else False,
            "operations": 120 if shadow else 0,
            "verifications": 120 if shadow else 0,
            "mismatches": mismatches,
            "first_mismatch": "none",
        },
    }


def semantic_hash(result: dict) -> str:
    view = dict(result)
    view.pop("elapsed_ms", None)
    view.pop("shadow", None)
    return canonical_sha(view)


def make_report(platform: str, *, include_faults: bool | None = None) -> dict:
    baseline_result = sample_result(shadow=False)
    shadow_result = sample_result(shadow=True)
    semantic = semantic_hash(baseline_result)
    baseline = [
        {
            "seconds": value,
            "peak_rss_bytes": 10_000_000,
            "result": copy.deepcopy(baseline_result),
            "semantic_sha256": semantic,
        }
        for value in (1.0, 1.1, 1.2)
    ]
    shadow = [
        {
            "seconds": value,
            "peak_rss_bytes": 12_000_000,
            "result": copy.deepcopy(shadow_result),
            "semantic_sha256": semantic,
        }
        for value in (1.1, 1.2, 1.3)
    ]
    faults = {}
    if include_faults is None:
        include_faults = platform == "linux"
    if include_faults:
        for name, mismatch in certify_mod.FAULT_EXPECTATIONS.items():
            result = sample_result(shadow=True, healthy=False, mismatches=1)
            result["shadow"]["first_mismatch"] = mismatch
            faults[name] = {
                "seconds": 1.0,
                "peak_rss_bytes": 12_000_000,
                "result": result,
                "semantic_sha256": semantic,
            }

    report = {
        "schema": "zevryon.z2r3du.paired-report.v1",
        "platform": platform,
        "logical_bytes": 16 * 1024 * 1024,
        "rounds": 2,
        "samples": 3,
        "baseline": baseline,
        "shadow": shadow,
        "faults": faults,
        "semantic_sha256": semantic,
        "seconds": {
            "baseline": {
                "minimum": 1.0,
                "p50": 1.1,
                "p95": 1.2,
                "p99": 1.2,
                "maximum": 1.2,
                "mean": 1.1,
            },
            "shadow": {
                "minimum": 1.1,
                "p50": 1.2,
                "p95": 1.3,
                "p99": 1.3,
                "maximum": 1.3,
                "mean": 1.2,
            },
        },
        "peak_rss_bytes": {
            "baseline": {
                "minimum": 10_000_000,
                "p50": 10_000_000,
                "p95": 10_000_000,
                "p99": 10_000_000,
                "maximum": 10_000_000,
                "mean": 10_000_000,
            },
            "shadow": {
                "minimum": 12_000_000,
                "p50": 12_000_000,
                "p95": 12_000_000,
                "p99": 12_000_000,
                "maximum": 12_000_000,
                "mean": 12_000_000,
            },
        },
    }
    report["report_sha256"] = certify_mod.report_sha256(report)
    return report


def certify(platform: str) -> dict:
    return certify_mod.certify_report(
        make_report(platform),
        commit_sha="a" * 40,
        compiler="test",
        build_type="Release",
        max_p50_ratio=2.0,
        max_p95_ratio=2.25,
        max_p99_ratio=2.5,
        max_maximum_ratio=3.0,
        max_memory_ratio=1.5,
        max_memory_delta_bytes=16 * 1024 * 1024,
    )


def test_valid_three_platform_chain() -> None:
    manifests = [certify(platform) for platform in ("linux", "windows", "macos")]
    result = finalize_mod.finalize(
        manifests,
        commit_sha="a" * 40,
        prerequisite_head="b" * 40,
    )
    assert result["promotion_ready"] is True
    assert result["all_platforms_ready"] is True
    assert result["authority"]["switch_performed"] is False
    assert result["manifest_sha256"] == finalize_mod.manifest_sha256(result)


def test_semantic_divergence_fails_closed() -> None:
    report = make_report("linux")
    report["shadow"][0]["semantic_sha256"] = "f" * 64
    report["report_sha256"] = certify_mod.report_sha256(report)
    with pytest.raises(ValueError, match="semantic"):
        certify_mod.certify_report(
            report,
            commit_sha="a" * 40,
            compiler="test",
            build_type="Release",
            max_p50_ratio=2.0,
            max_p95_ratio=2.25,
            max_p99_ratio=2.5,
            max_maximum_ratio=3.0,
            max_memory_ratio=1.5,
            max_memory_delta_bytes=16 * 1024 * 1024,
        )


def test_positive_shadow_mismatch_fails_closed() -> None:
    report = make_report("linux")
    report["shadow"][0]["result"]["shadow"]["mismatches"] = 1
    report["shadow"][0]["result"]["shadow"]["healthy"] = False
    report["report_sha256"] = certify_mod.report_sha256(report)
    with pytest.raises(ValueError, match="unhealthy|mismatch"):
        certify_mod.certify_report(
            report,
            commit_sha="a" * 40,
            compiler="test",
            build_type="Release",
            max_p50_ratio=2.0,
            max_p95_ratio=2.25,
            max_p99_ratio=2.5,
            max_maximum_ratio=3.0,
            max_memory_ratio=1.5,
            max_memory_delta_bytes=16 * 1024 * 1024,
        )


def test_missing_fault_class_fails_closed() -> None:
    report = make_report("linux")
    del report["faults"]["reset"]
    report["report_sha256"] = certify_mod.report_sha256(report)
    with pytest.raises(ValueError, match="four Unicode fault classes"):
        certify_mod.certify_report(
            report,
            commit_sha="a" * 40,
            compiler="test",
            build_type="Release",
            max_p50_ratio=2.0,
            max_p95_ratio=2.25,
            max_p99_ratio=2.5,
            max_maximum_ratio=3.0,
            max_memory_ratio=1.5,
            max_memory_delta_bytes=16 * 1024 * 1024,
        )


def test_performance_regression_fails_closed() -> None:
    report = make_report("windows")
    report["seconds"]["shadow"]["p50"] = 5.0
    report["report_sha256"] = certify_mod.report_sha256(report)
    with pytest.raises(ValueError, match="p50"):
        certify_mod.certify_report(
            report,
            commit_sha="a" * 40,
            compiler="test",
            build_type="Release",
            max_p50_ratio=2.0,
            max_p95_ratio=2.25,
            max_p99_ratio=2.5,
            max_maximum_ratio=3.0,
            max_memory_ratio=1.5,
            max_memory_delta_bytes=16 * 1024 * 1024,
        )


def test_forged_report_hash_fails_closed() -> None:
    report = make_report("macos")
    report["logical_bytes"] += 1
    with pytest.raises(ValueError, match="SHA-256"):
        certify_mod.certify_report(
            report,
            commit_sha="a" * 40,
            compiler="test",
            build_type="Release",
            max_p50_ratio=2.0,
            max_p95_ratio=2.25,
            max_p99_ratio=2.5,
            max_maximum_ratio=3.0,
            max_memory_ratio=1.5,
            max_memory_delta_bytes=16 * 1024 * 1024,
        )


def test_finalizer_rejects_mixed_commits() -> None:
    manifests = [certify(platform) for platform in ("linux", "windows", "macos")]
    manifests[1]["commit_sha"] = "c" * 40
    manifests[1]["manifest_sha256"] = finalize_mod.manifest_sha256(manifests[1])
    with pytest.raises(ValueError, match="commit"):
        finalize_mod.finalize(
            manifests,
            commit_sha="a" * 40,
            prerequisite_head="b" * 40,
        )
