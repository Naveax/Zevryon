from __future__ import annotations

import copy
import importlib.util
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


cert = load_module(
    "z2r1d4_platform_certification",
    ROOT / "scripts" / "z2r1d4_platform_certification.py",
)
finalize = load_module(
    "z2r1d4_finalize_promotion",
    ROOT / "scripts" / "z2r1d4_finalize_promotion.py",
)


def make_probe() -> dict:
    return {
        "schema": "zevryon.rust-shadow-workload-probe.v1",
        "resource_class_count": 36,
        "rust_shadow_enabled": True,
        "rust_shadow_healthy": True,
        "rust_shadow_mismatches": 0,
        "total_current_bytes": 0,
        "within_hard_limits": True,
        "accounting_clean": True,
        "rust_shadow_operations": 360,
        "rust_shadow_verifications": 6,
        "trace_checksum": "49173f798a426b3d",
    }


def make_run(platform: str, mode: str) -> dict:
    test_names = [f"test-{index}" for index in range(8)] + [
        "directwrite-discovery-tests" if platform == "windows" else "coretext-discovery-tests"
    ]
    workloads = []
    for index, name in enumerate(cert.EXPECTED_WORKLOADS):
        workloads.append(
            {
                "name": name,
                "sample_count": 3,
                "semantic_sha256": f"{index + 1:064x}",
                "median_p50_ms": 1.0 if mode == "baseline" else 1.1,
                "median_p95_ms": 1.2 if mode == "baseline" else 1.25,
                "median_wall_seconds": 2.0 if mode == "baseline" else 2.1,
                "median_peak_rss_bytes": 1000 if mode == "baseline" else 1100,
                "accounting_clean": True,
                "within_hard_limits": True,
            }
        )
    return {
        "schema": cert.RUN_SCHEMA,
        "platform": platform,
        "adapter": cert.EXPECTED_ADAPTER[platform],
        "mode": mode,
        "tests": {
            "count": len(test_names),
            "names": test_names,
            "names_sha256": "a" * 64,
        },
        "workloads": workloads,
        "rust_shadow_probe": make_probe() if mode == "shadow" else None,
    }


def platform_manifest(platform: str, commit: str = "c" * 40) -> dict:
    return cert.build_manifest(
        make_run(platform, "baseline"),
        make_run(platform, "shadow"),
        platform_name=platform,
        commit_sha=commit,
        compiler="test-compiler",
        build_type="Release",
        max_p50_ratio=1.50,
        max_p95_ratio=1.50,
        max_wall_ratio=1.75,
        max_peak_rss_ratio=1.50,
    )


def test_platform_manifest_success() -> None:
    report = platform_manifest("windows")
    assert report["slice_ready"] is True
    assert report["promotion_ready"] is False
    assert report["probe"]["mismatches"] == 0
    assert len(report["workloads"]) == 4


def test_platform_manifest_rejects_semantic_divergence() -> None:
    baseline = make_run("macos", "baseline")
    shadow = make_run("macos", "shadow")
    shadow["workloads"][0]["semantic_sha256"] = "f" * 64
    try:
        cert.build_manifest(
            baseline,
            shadow,
            platform_name="macos",
            commit_sha="c" * 40,
            compiler="test",
            build_type="Release",
            max_p50_ratio=1.50,
            max_p95_ratio=1.50,
            max_wall_ratio=1.75,
            max_peak_rss_ratio=1.50,
        )
    except cert.CertificationError as error:
        assert "semantic output diverged" in str(error)
    else:
        raise AssertionError("semantic divergence was accepted")


def test_platform_manifest_rejects_overhead_gate() -> None:
    baseline = make_run("windows", "baseline")
    shadow = make_run("windows", "shadow")
    shadow["workloads"][2]["median_p50_ms"] = 2.0
    try:
        cert.build_manifest(
            baseline,
            shadow,
            platform_name="windows",
            commit_sha="c" * 40,
            compiler="test",
            build_type="Release",
            max_p50_ratio=1.50,
            max_p95_ratio=1.50,
            max_wall_ratio=1.75,
            max_peak_rss_ratio=1.50,
        )
    except cert.CertificationError as error:
        assert "performance or memory gate" in str(error)
    else:
        raise AssertionError("overhead regression was accepted")


def test_platform_manifest_rejects_probe_mismatch() -> None:
    shadow = make_run("macos", "shadow")
    shadow["rust_shadow_probe"]["rust_shadow_mismatches"] = 1
    try:
        cert.build_manifest(
            make_run("macos", "baseline"),
            shadow,
            platform_name="macos",
            commit_sha="c" * 40,
            compiler="test",
            build_type="Release",
            max_p50_ratio=1.50,
            max_p95_ratio=1.50,
            max_wall_ratio=1.75,
            max_peak_rss_ratio=1.50,
        )
    except cert.CertificationError as error:
        assert "probe recorded a mismatch" in str(error)
    else:
        raise AssertionError("Rust mismatch was accepted")


def test_final_manifest_success_without_switching_authority() -> None:
    commit = "d" * 40
    report = finalize.build_final_manifest(
        platform_manifest("windows", commit),
        platform_manifest("macos", commit),
        commit_sha=commit,
        prerequisites=copy.deepcopy(finalize.EXPECTED_PREREQUISITES),
    )
    assert report["promotion_ready"] is True
    assert report["all_mandatory_slices_ready"] is True
    assert report["authority"]["authoritative_switch_performed"] is False
    assert len(report["mandatory_slices"]) == 5


def test_final_manifest_rejects_prerequisite_change() -> None:
    commit = "e" * 40
    prerequisites = copy.deepcopy(finalize.EXPECTED_PREREQUISITES)
    prerequisites["Z2R-1D2-unicode-text"] = "0" * 64
    try:
        finalize.build_final_manifest(
            platform_manifest("windows", commit),
            platform_manifest("macos", commit),
            commit_sha=commit,
            prerequisites=prerequisites,
        )
    except finalize.FinalizationError as error:
        assert "prerequisite certification set mismatch" in str(error)
    else:
        raise AssertionError("changed prerequisite was accepted")


def test_final_manifest_requires_same_commit() -> None:
    try:
        finalize.build_final_manifest(
            platform_manifest("windows", "1" * 40),
            platform_manifest("macos", "2" * 40),
            commit_sha="1" * 40,
            prerequisites=copy.deepcopy(finalize.EXPECTED_PREREQUISITES),
        )
    except finalize.FinalizationError as error:
        assert "macOS commit SHA mismatch" in str(error)
    else:
        raise AssertionError("mixed platform commits were accepted")
