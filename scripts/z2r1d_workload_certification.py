#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
from typing import Any


class CertificationError(RuntimeError):
    pass


def load_json(path: Path) -> dict[str, Any]:
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise CertificationError(f"cannot read JSON report {path}: {error}") from error
    if not isinstance(payload, dict):
        raise CertificationError(f"report {path} is not a JSON object")
    return payload


def canonical_bytes(payload: Any) -> bytes:
    return json.dumps(payload, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode(
        "utf-8"
    )


def canonical_sha256(payload: Any) -> str:
    return hashlib.sha256(canonical_bytes(payload)).hexdigest()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise CertificationError(message)


def finite_non_negative(value: Any, label: str) -> float:
    try:
        number = float(value)
    except (TypeError, ValueError) as error:
        raise CertificationError(f"{label} is not numeric") from error
    if not math.isfinite(number) or number < 0.0:
        raise CertificationError(f"{label} must be finite and non-negative")
    return number


def ratio(shadow: float, baseline: float, label: str) -> float:
    if baseline <= 0.0:
        raise CertificationError(f"{label} baseline must be positive")
    return shadow / baseline


def measured_seconds(measurement: dict[str, Any], label: str) -> float:
    return finite_non_negative(measurement.get("seconds"), f"{label}.seconds")


def measured_peak_pss(measurement: dict[str, Any], label: str) -> int | None:
    value = measurement.get("peak_pss_bytes")
    if value is None:
        return None
    try:
        parsed = int(value)
    except (TypeError, ValueError) as error:
        raise CertificationError(f"{label}.peak_pss_bytes is not an integer") from error
    if parsed < 0:
        raise CertificationError(f"{label}.peak_pss_bytes is negative")
    return parsed


def semantic_hash_massivedoc(report: dict[str, Any]) -> str:
    semantic = {
        "logical_bytes": report["logical_bytes"],
        "logical_records": report["logical_records"],
        "logical_nodes": report["logical_nodes"],
        "style_runs": report["style_runs"],
        "resource_references": report["resource_references"],
        "largest_record_observed_bytes": report["largest_record_observed_bytes"],
        "giant_record_bytes": report["giant_record_bytes"],
        "payload_sha256": report["payload_sha256"],
        "export_sha256": report["export_sha256"],
        "bounded_viewport_materialization": report["bounded_viewport_materialization"],
        "bounded_layout_fragment_materialization": report[
            "bounded_layout_fragment_materialization"
        ],
        "zero_data_loss": report["zero_data_loss"],
        "tail_marker_in_final_record": report["tail_marker_in_final_record"],
    }
    return canonical_sha256(semantic)


def massivedoc_metrics(report: dict[str, Any], label: str) -> dict[str, Any]:
    require(
        report.get("schema") == "zevryon.massivedoc.benchmark.v4",
        f"{label} has the wrong MassiveDoc schema",
    )
    require(report.get("zero_data_loss") is True, f"{label} did not certify zero data loss")
    require(
        report.get("payload_sha256") == report.get("export_sha256"),
        f"{label} payload/export checksum mismatch",
    )
    measurements: list[tuple[str, dict[str, Any]]] = []
    for stage in ("import", "search", "verify", "export", "arena_build"):
        item = report.get(stage)
        require(isinstance(item, dict), f"{label}.{stage} is missing")
        measurements.append((stage, item))
    viewports = report.get("viewports")
    require(isinstance(viewports, dict), f"{label}.viewports is missing")
    for name in ("top", "middle", "end"):
        item = viewports.get(name)
        require(isinstance(item, dict), f"{label}.viewports.{name} is missing")
        measurements.append((f"viewport_{name}", item))

    wall_seconds = sum(measured_seconds(item, f"{label}.{name}") for name, item in measurements)
    peaks = [
        peak
        for name, item in measurements
        if (peak := measured_peak_pss(item, f"{label}.{name}")) is not None
    ]
    return {
        "wall_seconds": wall_seconds,
        "peak_pss_bytes": max(peaks) if peaks else None,
        "semantic_sha256": semantic_hash_massivedoc(report),
        "payload_sha256": str(report["payload_sha256"]),
    }


def semantic_hash_global_layout(report: dict[str, Any]) -> str:
    measurement = report["measurement"]
    result = measurement["result"]
    layout = result["layout"]
    semantic = {
        "expected_record": report["expected_record"],
        "scroll_y_px": report["scroll_y_px"],
        "max_source_bytes": report["max_source_bytes"],
        "checkpoint_accelerated": report["checkpoint_accelerated"],
        "bounded_global_random_access": report["bounded_global_random_access"],
        "checkpoint_hits": layout["checkpoint_hits"],
        "source_bytes_read": layout["source_bytes_read"],
        "fragments": layout["fragments"],
    }
    return canonical_sha256(semantic)


def global_layout_metrics(report: dict[str, Any], label: str) -> dict[str, Any]:
    require(
        report.get("schema") == "zevryon.zenith.global-layout.v1",
        f"{label} has the wrong global-layout schema",
    )
    require(report.get("checkpoint_accelerated") is True, f"{label} missed checkpoints")
    require(
        report.get("bounded_global_random_access") is True,
        f"{label} is not bounded global random access",
    )
    measurement = report.get("measurement")
    require(isinstance(measurement, dict), f"{label}.measurement is missing")
    return {
        "wall_seconds": measured_seconds(measurement, f"{label}.measurement"),
        "peak_pss_bytes": measured_peak_pss(measurement, f"{label}.measurement"),
        "semantic_sha256": semantic_hash_global_layout(report),
    }


def semantic_hash_hot_scroll(report: dict[str, Any]) -> str:
    result = report["hot_scroll"]["result"]
    semantic = {
        "expected_record": report["expected_record"],
        "viewport_width_px": report["viewport_width_px"],
        "viewport_height_px": report["viewport_height_px"],
        "queries_per_profile": report["queries_per_profile"],
        "stride_bytes": report["stride_bytes"],
        "checkpoint_index_bytes": report["checkpoint_index_bytes"],
        "bounded_checkpoint_cache": report["bounded_checkpoint_cache"],
        "bounded_source_window_cache": report["bounded_source_window_cache"],
        "zero_payload_data_loss": report["zero_payload_data_loss"],
        "random_maximum_source_bytes_read": result["random"]["maximum_source_bytes_read"],
        "adjacent_maximum_source_bytes_read": result["adjacent"][
            "maximum_source_bytes_read"
        ],
        "random_zero_source_read_queries": result["random"]["zero_source_read_queries"],
        "adjacent_zero_source_read_queries": result["adjacent"][
            "zero_source_read_queries"
        ],
        "random_checkpoint_cache_misses": result["random"]["checkpoint_cache_misses"],
        "adjacent_checkpoint_cache_misses": result["adjacent"][
            "checkpoint_cache_misses"
        ],
    }
    return canonical_sha256(semantic)


def hot_scroll_metrics(report: dict[str, Any], label: str) -> dict[str, Any]:
    require(
        report.get("schema") == "zevryon.zenith.hot-scroll.v1",
        f"{label} has the wrong hot-scroll schema",
    )
    for field in (
        "bounded_checkpoint_cache",
        "bounded_source_window_cache",
        "zero_payload_data_loss",
    ):
        require(report.get(field) is True, f"{label}.{field} is not true")
    measured = report.get("hot_scroll")
    require(isinstance(measured, dict), f"{label}.hot_scroll is missing")
    result = measured.get("result")
    require(isinstance(result, dict), f"{label}.hot_scroll.result is missing")
    return {
        "wall_seconds": measured_seconds(measured, f"{label}.hot_scroll"),
        "peak_pss_bytes": measured_peak_pss(measured, f"{label}.hot_scroll"),
        "random_p95_ms": finite_non_negative(
            result["random"]["p95_ms"], f"{label}.random.p95_ms"
        ),
        "adjacent_p95_ms": finite_non_negative(
            result["adjacent"]["p95_ms"], f"{label}.adjacent.p95_ms"
        ),
        "semantic_sha256": semantic_hash_hot_scroll(report),
    }


def validate_probe(probe: dict[str, Any]) -> dict[str, Any]:
    require(
        probe.get("schema") == "zevryon.rust-shadow-workload-probe.v1",
        "Rust shadow probe schema mismatch",
    )
    require(probe.get("resource_class_count") == 36, "probe did not cover all resource classes")
    require(probe.get("rust_shadow_enabled") is True, "probe Rust shadow is disabled")
    require(probe.get("rust_shadow_healthy") is True, "probe Rust shadow is unhealthy")
    require(probe.get("rust_shadow_mismatches") == 0, "probe recorded a Rust mismatch")
    require(probe.get("total_current_bytes") == 0, "probe leaked current bytes")
    require(probe.get("within_hard_limits") is True, "probe exceeded a hard limit")
    require(probe.get("accounting_clean") is True, "probe accounting is dirty")
    workloads = probe.get("workloads")
    require(isinstance(workloads, list), "probe workloads are missing")
    names = [item.get("name") for item in workloads if isinstance(item, dict)]
    require(
        names == ["massivedoc", "layout", "unicode", "font", "browser"],
        "probe workload groups are incomplete or out of order",
    )
    require(
        sum(int(item["resource_classes"]) for item in workloads) == 36,
        "probe workload resource-class total is not 36",
    )
    return {
        "trace_checksum": str(probe["trace_checksum"]),
        "operations": int(probe["rust_shadow_operations"]),
        "verifications": int(probe["rust_shadow_verifications"]),
        "mismatches": int(probe["rust_shadow_mismatches"]),
        "total_peak_bytes": int(probe["total_peak_bytes"]),
        "semantic_sha256": canonical_sha256(
            {
                "trace_checksum": probe["trace_checksum"],
                "workloads": workloads,
                "operations": probe["rust_shadow_operations"],
                "verifications": probe["rust_shadow_verifications"],
            }
        ),
    }


def compare_pair(
    name: str,
    baseline: dict[str, Any],
    shadow: dict[str, Any],
    *,
    max_wall_time_ratio: float,
    max_peak_pss_ratio: float,
) -> dict[str, Any]:
    checksum_equal = baseline["semantic_sha256"] == shadow["semantic_sha256"]
    wall_ratio = ratio(
        float(shadow["wall_seconds"]),
        float(baseline["wall_seconds"]),
        f"{name}.wall",
    )
    baseline_peak = baseline.get("peak_pss_bytes")
    shadow_peak = shadow.get("peak_pss_bytes")
    peak_ratio: float | None = None
    peak_gate = True
    if baseline_peak is not None and shadow_peak is not None:
        peak_ratio = ratio(float(shadow_peak), float(baseline_peak), f"{name}.peak_pss")
        peak_gate = peak_ratio <= max_peak_pss_ratio
    return {
        "name": name,
        "baseline": baseline,
        "shadow": shadow,
        "checksum_parity": checksum_equal,
        "wall_time_ratio": wall_ratio,
        "wall_time_gate": wall_ratio <= max_wall_time_ratio,
        "peak_pss_ratio": peak_ratio,
        "peak_pss_gate": peak_gate,
        "passed": checksum_equal and wall_ratio <= max_wall_time_ratio and peak_gate,
    }


def build_manifest(
    *,
    probe: dict[str, Any],
    baseline_massivedoc: dict[str, Any],
    shadow_massivedoc: dict[str, Any],
    baseline_global_layout: dict[str, Any],
    shadow_global_layout: dict[str, Any],
    baseline_hot_scroll: dict[str, Any],
    shadow_hot_scroll: dict[str, Any],
    commit_sha: str,
    compiler: str,
    build_type: str,
    runner_os: str,
    rust_abi_version: str,
    max_wall_time_ratio: float,
    max_peak_pss_ratio: float,
) -> dict[str, Any]:
    require(max_wall_time_ratio >= 1.0, "max wall-time ratio must be at least 1.0")
    require(max_peak_pss_ratio >= 1.0, "max peak-PSS ratio must be at least 1.0")

    probe_metrics = validate_probe(probe)
    massivedoc = compare_pair(
        "massivedoc_giant_record",
        massivedoc_metrics(baseline_massivedoc, "baseline_massivedoc"),
        massivedoc_metrics(shadow_massivedoc, "shadow_massivedoc"),
        max_wall_time_ratio=max_wall_time_ratio,
        max_peak_pss_ratio=max_peak_pss_ratio,
    )
    require(
        massivedoc["baseline"]["payload_sha256"]
        == massivedoc["shadow"]["payload_sha256"],
        "baseline/shadow MassiveDoc payload checksums differ",
    )
    global_layout = compare_pair(
        "zenith_global_layout",
        global_layout_metrics(baseline_global_layout, "baseline_global_layout"),
        global_layout_metrics(shadow_global_layout, "shadow_global_layout"),
        max_wall_time_ratio=max_wall_time_ratio,
        max_peak_pss_ratio=max_peak_pss_ratio,
    )
    hot_scroll = compare_pair(
        "zenith_hot_scroll",
        hot_scroll_metrics(baseline_hot_scroll, "baseline_hot_scroll"),
        hot_scroll_metrics(shadow_hot_scroll, "shadow_hot_scroll"),
        max_wall_time_ratio=max_wall_time_ratio,
        max_peak_pss_ratio=max_peak_pss_ratio,
    )

    workload_pairs = [massivedoc, global_layout, hot_scroll]
    checksums_exact = all(item["checksum_parity"] for item in workload_pairs)
    overhead_within_gate = all(
        item["wall_time_gate"] and item["peak_pss_gate"] for item in workload_pairs
    )
    slice_ready = checksums_exact and overhead_within_gate and probe_metrics["mismatches"] == 0
    blocked_by = [
        "unicode_grapheme_bidi_script_real_workload_pair_not_yet_measured",
        "font_discovery_fallback_shaping_real_workload_pair_not_yet_measured",
        "windows_and_macos_workload_overhead_not_yet_measured",
    ]

    manifest = {
        "schema": "zevryon.rust-shadow-certification.v1",
        "slice": "Z2R-1D1-massivedoc-zenith",
        "build": {
            "commit_sha": commit_sha,
            "compiler": compiler,
            "build_type": build_type,
            "runner_os": runner_os,
            "rust_abi_version": rust_abi_version,
            "rollback_configuration": {
                "ZEVRYON_ENABLE_RUST_CORE": "OFF",
                "ZEVRYON_RUST_LEDGER_SHADOW": "OFF",
            },
        },
        "input_sha256": {
            "probe": canonical_sha256(probe),
            "baseline_massivedoc": canonical_sha256(baseline_massivedoc),
            "shadow_massivedoc": canonical_sha256(shadow_massivedoc),
            "baseline_global_layout": canonical_sha256(baseline_global_layout),
            "shadow_global_layout": canonical_sha256(shadow_global_layout),
            "baseline_hot_scroll": canonical_sha256(baseline_hot_scroll),
            "shadow_hot_scroll": canonical_sha256(shadow_hot_scroll),
        },
        "probe": probe_metrics,
        "workloads": workload_pairs,
        "gates": {
            "max_wall_time_ratio": max_wall_time_ratio,
            "max_peak_pss_ratio": max_peak_pss_ratio,
            "checksum_parity": checksums_exact,
            "shadow_mismatches_zero": probe_metrics["mismatches"] == 0,
            "overhead_within_gate": overhead_within_gate,
            "rollback_available": True,
        },
        "slice_ready": slice_ready,
        "promotion_ready": False,
        "blocked_by": blocked_by,
    }
    manifest["manifest_sha256"] = canonical_sha256(manifest)
    return manifest


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Build the Z2R-1D production Rust-shadow workload certification manifest"
    )
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--baseline-massivedoc", type=Path, required=True)
    parser.add_argument("--shadow-massivedoc", type=Path, required=True)
    parser.add_argument("--baseline-global-layout", type=Path, required=True)
    parser.add_argument("--shadow-global-layout", type=Path, required=True)
    parser.add_argument("--baseline-hot-scroll", type=Path, required=True)
    parser.add_argument("--shadow-hot-scroll", type=Path, required=True)
    parser.add_argument("--commit-sha", required=True)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--build-type", default="Release")
    parser.add_argument("--runner-os", required=True)
    parser.add_argument("--rust-abi-version", default="1")
    parser.add_argument("--max-wall-time-ratio", type=float, default=2.0)
    parser.add_argument("--max-peak-pss-ratio", type=float, default=1.5)
    parser.add_argument("--output", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        manifest = build_manifest(
            probe=load_json(args.probe),
            baseline_massivedoc=load_json(args.baseline_massivedoc),
            shadow_massivedoc=load_json(args.shadow_massivedoc),
            baseline_global_layout=load_json(args.baseline_global_layout),
            shadow_global_layout=load_json(args.shadow_global_layout),
            baseline_hot_scroll=load_json(args.baseline_hot_scroll),
            shadow_hot_scroll=load_json(args.shadow_hot_scroll),
            commit_sha=args.commit_sha,
            compiler=args.compiler,
            build_type=args.build_type,
            runner_os=args.runner_os,
            rust_abi_version=args.rust_abi_version,
            max_wall_time_ratio=args.max_wall_time_ratio,
            max_peak_pss_ratio=args.max_peak_pss_ratio,
        )
    except CertificationError as error:
        print(f"Z2R-1D certification failed: {error}")
        return 1

    text = json.dumps(manifest, indent=2, sort_keys=True) + "\n"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(text, encoding="utf-8")
    print(text, end="")
    return 0 if manifest["slice_ready"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
