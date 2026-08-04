from __future__ import annotations

import importlib.util
from pathlib import Path
import sys

MODULE_PATH = Path(__file__).parents[1] / "scripts" / "z2r1d3_font_certification.py"
SPEC = importlib.util.spec_from_file_location("z2r1d3_font_certification", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


def run(mode: str) -> dict:
    workloads = []
    for index, name in enumerate(MODULE.EXPECTED_WORKLOADS, start=1):
        workloads.append(
            {
                "name": name,
                "sample_count": 3,
                "semantic_sha256": f"{index:064x}",
                "median_p50_ms": float(index),
                "median_p95_ms": float(index) * 1.1,
                "median_wall_seconds": float(index) / 10.0,
                "median_peak_pss_bytes": 1_000_000 + index,
                "accounting_clean": True,
                "within_hard_limits": True,
            }
        )
    names = [f"test-{index}" for index in range(23)]
    return {
        "schema": MODULE.RUN_SCHEMA,
        "mode": mode,
        "fonts": {
            "latin": {"bytes": 100, "sha256": "a" * 64},
            "devanagari": {"bytes": 200, "sha256": "b" * 64},
        },
        "tests": {"count": len(names), "names": names, "names_sha256": "c" * 64},
        "workloads": workloads,
    }


def probe() -> dict:
    return {
        "schema": "zevryon.rust-shadow-workload-probe.v1",
        "resource_class_count": 36,
        "trace_checksum": "abc",
        "rust_shadow_enabled": True,
        "rust_shadow_healthy": True,
        "rust_shadow_operations": 100,
        "rust_shadow_verifications": 10,
        "rust_shadow_mismatches": 0,
        "total_current_bytes": 0,
        "within_hard_limits": True,
        "accounting_clean": True,
    }


def manifest(baseline: dict, shadow: dict, shadow_probe: dict) -> dict:
    return MODULE.build_manifest(
        baseline,
        shadow,
        shadow_probe,
        commit_sha="d" * 40,
        compiler="compiler",
        build_type="Release",
        runner_os="Linux",
        max_p50_ratio=1.5,
        max_p95_ratio=1.5,
        max_wall_ratio=1.75,
        max_peak_pss_ratio=1.5,
    )


def test_success_manifest_is_slice_ready_but_not_promoted() -> None:
    result = manifest(run("baseline"), run("shadow"), probe())
    assert result["slice_ready"] is True
    assert result["promotion_ready"] is False
    assert len(result["workloads"]) == len(MODULE.EXPECTED_WORKLOADS)
    assert len(result["manifest_sha256"]) == 64


def test_semantic_divergence_fails_closed() -> None:
    baseline = run("baseline")
    shadow = run("shadow")
    shadow["workloads"][3]["semantic_sha256"] = "f" * 64
    try:
        manifest(baseline, shadow, probe())
    except MODULE.CertificationError as error:
        assert "semantic output diverged" in str(error)
    else:
        raise AssertionError("semantic divergence was accepted")


def test_shadow_mismatch_fails_closed() -> None:
    bad_probe = probe()
    bad_probe["rust_shadow_mismatches"] = 1
    try:
        manifest(run("baseline"), run("shadow"), bad_probe)
    except MODULE.CertificationError as error:
        assert "recorded a mismatch" in str(error)
    else:
        raise AssertionError("shadow mismatch was accepted")


def test_performance_regression_fails_closed() -> None:
    baseline = run("baseline")
    shadow = run("shadow")
    shadow["workloads"][0]["median_p95_ms"] = 999.0
    try:
        manifest(baseline, shadow, probe())
    except MODULE.CertificationError as error:
        assert "performance or memory gate" in str(error)
    else:
        raise AssertionError("performance regression was accepted")


def test_missing_workload_fails_closed() -> None:
    shadow = run("shadow")
    shadow["workloads"].pop()
    try:
        manifest(run("baseline"), shadow, probe())
    except MODULE.CertificationError as error:
        assert "workload order/scope mismatch" in str(error)
    else:
        raise AssertionError("missing workload was accepted")
