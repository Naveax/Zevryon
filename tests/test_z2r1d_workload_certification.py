from __future__ import annotations

import importlib.util
from pathlib import Path

import pytest


SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "z2r1d_workload_certification.py"
spec = importlib.util.spec_from_file_location("z2r1d_workload_certification", SCRIPT)
assert spec is not None and spec.loader is not None
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def measured(seconds: float, peak: int, result: dict) -> dict:
    return {
        "seconds": seconds,
        "peak_pss_bytes": peak,
        "result": result,
    }


def massivedoc(seconds_scale: float = 1.0, payload: str = "a" * 64) -> dict:
    stages = {
        name: measured(1.0 * seconds_scale, 10_000, {"operation": name})
        for name in ("import", "search", "verify", "export", "arena_build")
    }
    return {
        "schema": "zevryon.massivedoc.benchmark.v4",
        "logical_bytes": 1024,
        "logical_records": 16,
        "logical_nodes": 16,
        "style_runs": 4,
        "resource_references": 2,
        "largest_record_observed_bytes": 256,
        "giant_record_bytes": 256,
        "payload_sha256": payload,
        "export_sha256": payload,
        **stages,
        "viewports": {
            name: measured(0.1 * seconds_scale, 8_000, {"viewport": {"records": []}})
            for name in ("top", "middle", "end")
        },
        "bounded_viewport_materialization": True,
        "bounded_layout_fragment_materialization": True,
        "zero_data_loss": True,
        "tail_marker_in_final_record": True,
    }


def global_layout(seconds_scale: float = 1.0) -> dict:
    return {
        "schema": "zevryon.zenith.global-layout.v1",
        "expected_record": 8,
        "scroll_y_px": 100,
        "max_source_bytes": 2048,
        "checkpoint_accelerated": True,
        "bounded_global_random_access": True,
        "measurement": measured(
            0.5 * seconds_scale,
            12_000,
            {
                "layout": {
                    "checkpoint_hits": 2,
                    "source_bytes_read": 1024,
                    "fragments": [
                        {
                            "record_index": 8,
                            "source_start": 0,
                            "source_end": 64,
                            "y_q8": 0,
                            "height_q8": 256,
                        }
                    ],
                }
            },
        ),
    }


def hot_scroll(seconds_scale: float = 1.0) -> dict:
    profile = {
        "p95_ms": 0.4 * seconds_scale,
        "maximum_source_bytes_read": 4096,
        "zero_source_read_queries": 127,
        "checkpoint_cache_misses": 0,
    }
    return {
        "schema": "zevryon.zenith.hot-scroll.v1",
        "expected_record": 8,
        "viewport_width_px": 800,
        "viewport_height_px": 720,
        "queries_per_profile": 129,
        "stride_bytes": 16_384,
        "checkpoint_index_bytes": 8192,
        "bounded_checkpoint_cache": True,
        "bounded_source_window_cache": True,
        "zero_payload_data_loss": True,
        "hot_scroll": measured(
            0.8 * seconds_scale,
            14_000,
            {"random": dict(profile), "adjacent": dict(profile)},
        ),
    }


def probe() -> dict:
    groups = [
        ("massivedoc", 4),
        ("layout", 6),
        ("unicode", 10),
        ("font", 14),
        ("browser", 2),
    ]
    return {
        "schema": "zevryon.rust-shadow-workload-probe.v1",
        "resource_class_count": 36,
        "trace_checksum": "0123456789abcdef",
        "rust_shadow_enabled": True,
        "rust_shadow_healthy": True,
        "rust_shadow_operations": 360,
        "rust_shadow_verifications": 5,
        "rust_shadow_mismatches": 0,
        "total_current_bytes": 0,
        "total_peak_bytes": 123456,
        "within_hard_limits": True,
        "accounting_clean": True,
        "workloads": [
            {
                "name": name,
                "resource_classes": count,
                "operations": count * 10,
                "verifications": 1,
                "mismatches": 0,
                "current_bytes": 0,
                "peak_bytes": 123,
                "healthy": True,
            }
            for name, count in groups
        ],
    }


def build(**overrides):
    values = {
        "probe": probe(),
        "baseline_massivedoc": massivedoc(),
        "shadow_massivedoc": massivedoc(1.05),
        "baseline_global_layout": global_layout(),
        "shadow_global_layout": global_layout(1.05),
        "baseline_hot_scroll": hot_scroll(),
        "shadow_hot_scroll": hot_scroll(1.05),
        "commit_sha": "abc123",
        "compiler": "test-cxx",
        "build_type": "Release",
        "runner_os": "Linux",
        "rust_abi_version": "1",
        "max_wall_time_ratio": 2.0,
        "max_peak_pss_ratio": 1.5,
    }
    values.update(overrides)
    return module.build_manifest(**values)


def test_manifest_certifies_first_slice_but_not_authoritative_promotion() -> None:
    manifest = build()
    assert manifest["schema"] == "zevryon.rust-shadow-certification.v1"
    assert manifest["slice_ready"] is True
    assert manifest["promotion_ready"] is False
    assert manifest["gates"]["checksum_parity"] is True
    assert manifest["probe"]["mismatches"] == 0
    assert len(manifest["manifest_sha256"]) == 64


def test_payload_divergence_fails_closed() -> None:
    with pytest.raises(module.CertificationError, match="payload checksums differ"):
        build(shadow_massivedoc=massivedoc(1.0, payload="b" * 64))


def test_overhead_above_gate_marks_slice_not_ready() -> None:
    manifest = build(shadow_hot_scroll=hot_scroll(3.0))
    assert manifest["slice_ready"] is False
    hot = next(item for item in manifest["workloads"] if item["name"] == "zenith_hot_scroll")
    assert hot["wall_time_gate"] is False
