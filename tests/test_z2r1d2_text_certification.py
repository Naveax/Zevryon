from __future__ import annotations

import importlib.util
from pathlib import Path

import pytest


MODULE_PATH = Path(__file__).resolve().parents[1] / "scripts" / "z2r1d2_text_certification.py"
spec = importlib.util.spec_from_file_location("z2r1d2_text_certification", MODULE_PATH)
assert spec is not None and spec.loader is not None
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def stage(name: str, enabled: bool) -> dict:
    return {
        "name": name,
        "wall_ms": 0.1,
        "input_items": 100,
        "output_items": 80,
        "semantic_checksum": f"{name}-checksum",
        "current_bytes": 0,
        "peak_bytes": 4096,
        "within_hard_limits": True,
        "accounting_clean": True,
        "shadow_enabled": enabled,
        "shadow_exact": True,
        "shadow_healthy": enabled,
        "shadow_operations": 10 if enabled else 0,
        "shadow_verifications": 1 if enabled else 0,
        "shadow_mismatches": 0,
        "shadow": {"schema": "zevryon.rust-shadow-ledger.v1", "enabled": enabled},
    }


def probe(enabled: bool, seconds: float = 0.01) -> dict:
    return {
        "seconds": seconds,
        "peak_pss_bytes": 1_000_000,
        "result": {
            "schema": "zevryon.rust-shadow-text-probe.v1",
            "fixture_bytes": 65_536,
            "pipeline_checksum": "pipeline-checksum",
            "wall_ms": 1.0,
            "rust_shadow_enabled": enabled,
            "exact_verification": True,
            "rust_shadow_operations": 40 if enabled else 0,
            "rust_shadow_verifications": 4 if enabled else 0,
            "rust_shadow_mismatches": 0,
            "total_peak_bytes": 16_384,
            "stages": [stage(name, enabled) for name in ("unicode", "grapheme", "script", "bidi")],
        },
    }


def benchmark_result(name: str, multiplier: float = 1.0) -> dict:
    common = {
        "fixture_bytes": 65_536,
        "iterations": 1_024,
        "warmup_iterations": 32,
        "p50_ms": 0.10 * multiplier,
        "p95_ms": 0.20 * multiplier,
        "p99_ms": 0.30 * multiplier,
        "maximum_ms": 0.40 * multiplier,
        "p50_mib_per_second": 625.0 / multiplier,
        "rejected_reservations": 0,
        "accounting_errors": 0,
        "within_hard_limits": True,
        "accounting_clean": True,
    }
    if name == "unicode":
        return {
            **common,
            "schema": "zevryon.unicode-benchmark.v1",
            "chunk_bytes": 4096,
            "decoded_codepoints": 50_000,
            "unicode_hard_limit_bytes": 2_097_152,
            "unicode_current_bytes": 524_288,
            "unicode_peak_bytes": 524_288,
        }
    if name == "grapheme":
        return {
            **common,
            "schema": "zevryon.grapheme-benchmark.v1",
            "unicode_version": "17.0.0",
            "data_fingerprint": "grapheme-fingerprint",
            "boundary_record_bytes": 16,
            "input_codepoints": 50_000,
            "output_clusters": 40_000,
            "output_boundaries": 40_001,
            "suppressed_breaks": 10_000,
            "maximum_cluster_codepoints": 3,
            "maximum_cluster_source_bytes": 11,
            "cluster_hard_limit_bytes": 1_048_576,
            "cluster_current_bytes": 640_016,
            "cluster_peak_bytes": 640_016,
        }
    if name == "script":
        return {
            **common,
            "schema": "zevryon.script-run-benchmark.v1",
            "unicode_version": "17.0.0",
            "data_fingerprint": "script-fingerprint",
            "input_codepoints": 50_000,
            "input_clusters": 40_000,
            "output_runs": 5_000,
            "output_boundaries": 5_001,
            "boundary_record_bytes": 16,
            "neutral_clusters": 500,
            "explicit_extension_lookups": 100,
            "internal_cluster_conflicts": 0,
            "maximum_run_clusters": 20,
            "script_run_hard_limit_bytes": 262_144,
            "script_run_current_bytes": 80_016,
            "script_run_peak_bytes": 80_016,
        }
    return {
        **common,
        "schema": "zevryon.bidi-explicit-benchmark.v1",
        "unicode_version": "17.0.0",
        "data_fingerprint": "bidi-fingerprint",
        "input_codepoints": 50_000,
        "output_units": 50_000,
        "unit_record_bytes": 16,
        "paragraph_level": 0,
        "maximum_level": 4,
        "explicit_controls": 100,
        "isolate_initiators": 50,
        "fsi_resolutions": 10,
        "bidi_hard_limit_bytes": 1_048_576,
        "bidi_current_bytes": 800_000,
        "bidi_peak_bytes": 800_000,
    }


def conformance_result(name: str) -> dict:
    if name == "grapheme":
        return {
            "schema": "zevryon.grapheme-conformance.v1",
            "unicode_version": "17.0.0",
            "tests": 766,
            "codepoints": 1990,
            "clusters": 1391,
            "passed": True,
        }
    if name == "script":
        return {
            "schema": "zevryon.script-conformance.v1",
            "unicode_version": "17.0.0",
            "data_fingerprint": "script-fingerprint",
            "codepoints": 1_114_112,
            "script_ranges": 2287,
            "script_extension_ranges": 206,
            "explicit_extension_codepoints": 669,
            "passed": True,
        }
    return {
        "schema": "zevryon.bidi-conformance.v1",
        "unicode_version": "17.0.0",
        "codepoints": 1_114_112,
        "explicit_ranges": 1267,
        "missing_ranges": 1,
        "generated_ranges": 1267,
        "passed": True,
    }


def report(mode: str, multiplier: float = 1.0) -> dict:
    enabled = mode == "shadow"
    benchmarks = {
        name: [
            {
                "seconds": 1.0 * multiplier,
                "peak_pss_bytes": int(10_000_000 * multiplier),
                "result": benchmark_result(name, multiplier),
            }
            for _ in range(3)
        ]
        for name in ("unicode", "grapheme", "script", "bidi")
    }
    conformance = {
        name: {
            "seconds": 0.5 * multiplier,
            "peak_pss_bytes": int(5_000_000 * multiplier),
            "result": conformance_result(name),
        }
        for name in ("grapheme", "script", "bidi")
    }
    return {
        "schema": "zevryon.rust-shadow-text-workloads.v1",
        "mode": mode,
        "samples": 3,
        "ucd_sources_sha256": {
            "GraphemeBreakTest-17.0.0.txt": "a",
            "Scripts.txt": "b",
            "ScriptExtensions.txt": "c",
            "DerivedBidiClass.txt": "d",
            "PropertyValueAliases.txt": "e",
        },
        "probe": probe(enabled, 0.01 * multiplier),
        "benchmarks": benchmarks,
        "conformance": conformance,
    }


def certify(baseline: dict, shadow: dict) -> dict:
    return module.certify(
        baseline,
        shadow,
        commit_sha="abc123",
        runner_os="Linux",
        compiler="gcc",
        build_type="Release",
        maximum_p50_ratio=1.5,
        maximum_p95_ratio=1.5,
        maximum_wall_ratio=1.75,
        maximum_peak_pss_ratio=1.5,
    )


def test_valid_pair_is_slice_ready() -> None:
    manifest = certify(report("baseline"), report("shadow", 1.05))
    assert manifest["slice_ready"] is True
    assert manifest["promotion_ready"] is False
    assert manifest["probe"]["shadow_mismatches"] == 0
    assert manifest["workloads"]["unicode"]["gates"]["p50_ratio"] is True


def test_semantic_divergence_fails_closed() -> None:
    baseline = report("baseline")
    shadow = report("shadow")
    shadow["benchmarks"]["unicode"][0]["result"]["decoded_codepoints"] += 1
    with pytest.raises(module.CertificationError, match="semantic output"):
        certify(baseline, shadow)


def test_shadow_mismatch_fails_closed() -> None:
    shadow = report("shadow")
    shadow["probe"]["result"]["rust_shadow_mismatches"] = 1
    with pytest.raises(module.CertificationError, match="mismatch"):
        certify(report("baseline"), shadow)


def test_overhead_gate_fails_closed() -> None:
    with pytest.raises(module.CertificationError, match="overhead gate"):
        certify(report("baseline"), report("shadow", 1.8))
