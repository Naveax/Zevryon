#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import math
import statistics
from pathlib import Path
from typing import Any


class CertificationError(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise CertificationError(message)


def load_json(path: Path) -> dict[str, Any]:
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise CertificationError(f"cannot read JSON report {path}: {error}") from error
    require(isinstance(payload, dict), f"report {path} is not a JSON object")
    return payload


def canonical_bytes(payload: Any) -> bytes:
    return json.dumps(payload, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode("utf-8")


def canonical_sha256(payload: Any) -> str:
    return hashlib.sha256(canonical_bytes(payload)).hexdigest()


def finite_non_negative(value: Any, label: str) -> float:
    try:
        number = float(value)
    except (TypeError, ValueError) as error:
        raise CertificationError(f"{label} is not numeric") from error
    require(math.isfinite(number) and number >= 0.0, f"{label} must be finite and non-negative")
    return number


def positive_ratio(shadow: float, baseline: float, label: str) -> float:
    require(baseline > 0.0, f"{label} baseline must be positive")
    return shadow / baseline


def result_of(measurement: dict[str, Any], label: str) -> dict[str, Any]:
    result = measurement.get("result")
    require(isinstance(result, dict), f"{label}.result is missing")
    return result


def validate_report(report: dict[str, Any], mode: str) -> None:
    require(report.get("schema") == "zevryon.rust-shadow-text-workloads.v1", f"{mode} schema mismatch")
    require(report.get("mode") == mode, f"{mode} report mode mismatch")
    require(int(report.get("samples", 0)) >= 3, f"{mode} report has too few samples")
    sources = report.get("ucd_sources_sha256")
    require(isinstance(sources, dict) and len(sources) >= 5, f"{mode} UCD source set is incomplete")
    require(isinstance(report.get("probe"), dict), f"{mode} probe is missing")
    require(isinstance(report.get("benchmarks"), dict), f"{mode} benchmarks are missing")
    require(isinstance(report.get("conformance"), dict), f"{mode} conformance is missing")


def semantic_probe(report: dict[str, Any]) -> dict[str, Any]:
    probe = result_of(report["probe"], "probe")
    stages = probe.get("stages")
    require(isinstance(stages, list) and len(stages) == 4, "probe stages are incomplete")
    semantic_stages = []
    for stage in stages:
        require(isinstance(stage, dict), "probe stage is not an object")
        semantic_stages.append(
            {
                "name": stage.get("name"),
                "input_items": stage.get("input_items"),
                "output_items": stage.get("output_items"),
                "semantic_checksum": stage.get("semantic_checksum"),
                "current_bytes": stage.get("current_bytes"),
                "peak_bytes": stage.get("peak_bytes"),
                "within_hard_limits": stage.get("within_hard_limits"),
                "accounting_clean": stage.get("accounting_clean"),
            }
        )
    return {
        "fixture_bytes": probe.get("fixture_bytes"),
        "pipeline_checksum": probe.get("pipeline_checksum"),
        "stages": semantic_stages,
    }


def validate_probe(report: dict[str, Any], expected_enabled: bool, label: str) -> dict[str, Any]:
    measurement = report["probe"]
    probe = result_of(measurement, f"{label}.probe")
    require(probe.get("schema") == "zevryon.rust-shadow-text-probe.v1", f"{label} probe schema mismatch")
    require(probe.get("fixture_bytes") == 65_536, f"{label} probe fixture changed")
    require(probe.get("rust_shadow_enabled") is expected_enabled, f"{label} probe shadow state mismatch")
    require(probe.get("exact_verification") is True, f"{label} probe exact verification failed")
    require(probe.get("rust_shadow_mismatches") == 0, f"{label} probe recorded a mismatch")
    stages = probe.get("stages")
    require(isinstance(stages, list) and len(stages) == 4, f"{label} probe stages missing")
    require(
        [item.get("name") for item in stages if isinstance(item, dict)]
        == ["unicode", "grapheme", "script", "bidi"],
        f"{label} probe stage order changed",
    )
    for stage in stages:
        require(stage.get("current_bytes") == 0, f"{label}.{stage.get('name')} leaked bytes")
        require(stage.get("within_hard_limits") is True, f"{label}.{stage.get('name')} exceeded limits")
        require(stage.get("accounting_clean") is True, f"{label}.{stage.get('name')} accounting dirty")
        require(stage.get("shadow_enabled") is expected_enabled, f"{label}.{stage.get('name')} shadow state mismatch")
        require(stage.get("shadow_exact") is True, f"{label}.{stage.get('name')} exact verification failed")
        require(stage.get("shadow_mismatches") == 0, f"{label}.{stage.get('name')} mismatch recorded")
        if expected_enabled:
            require(stage.get("shadow_healthy") is True, f"{label}.{stage.get('name')} shadow unhealthy")
            require(int(stage.get("shadow_operations", 0)) > 0, f"{label}.{stage.get('name')} no shadow operations")
            require(int(stage.get("shadow_verifications", 0)) > 0, f"{label}.{stage.get('name')} no verification")
    if expected_enabled:
        require(int(probe.get("rust_shadow_operations", 0)) > 0, f"{label} probe had no operations")
        require(int(probe.get("rust_shadow_verifications", 0)) >= 4, f"{label} probe verification count too low")
    else:
        require(probe.get("rust_shadow_operations") == 0, f"{label} baseline unexpectedly used Rust")
    return {
        "semantic": semantic_probe(report),
        "semantic_sha256": canonical_sha256(semantic_probe(report)),
        "wall_seconds": finite_non_negative(measurement.get("seconds"), f"{label}.probe.seconds"),
        "peak_pss_bytes": measurement.get("peak_pss_bytes"),
        "operations": int(probe.get("rust_shadow_operations", 0)),
        "verifications": int(probe.get("rust_shadow_verifications", 0)),
        "mismatches": int(probe.get("rust_shadow_mismatches", 0)),
        "pipeline_checksum": str(probe.get("pipeline_checksum")),
    }


TIMING_FIELDS = {"p50_ms", "p95_ms", "p99_ms", "maximum_ms", "p50_mib_per_second"}


def benchmark_semantic(result: dict[str, Any]) -> dict[str, Any]:
    return {key: value for key, value in result.items() if key not in TIMING_FIELDS}


def benchmark_metrics(report: dict[str, Any], name: str, label: str) -> dict[str, Any]:
    samples = report["benchmarks"].get(name)
    require(isinstance(samples, list) and len(samples) >= 3, f"{label}.{name} samples missing")
    results = [result_of(sample, f"{label}.{name}[{index}]") for index, sample in enumerate(samples)]
    semantic_hashes = [canonical_sha256(benchmark_semantic(result)) for result in results]
    require(len(set(semantic_hashes)) == 1, f"{label}.{name} semantic output changed between samples")
    p50 = [finite_non_negative(result["p50_ms"], f"{label}.{name}.p50_ms") for result in results]
    p95 = [finite_non_negative(result["p95_ms"], f"{label}.{name}.p95_ms") for result in results]
    p99 = [finite_non_negative(result["p99_ms"], f"{label}.{name}.p99_ms") for result in results]
    maximum = [finite_non_negative(result["maximum_ms"], f"{label}.{name}.maximum_ms") for result in results]
    wall = [finite_non_negative(sample.get("seconds"), f"{label}.{name}.seconds") for sample in samples]
    pss_values = [
        int(sample["peak_pss_bytes"])
        for sample in samples
        if sample.get("peak_pss_bytes") is not None
    ]
    require(all(value >= 0 for value in pss_values), f"{label}.{name} peak PSS is negative")
    return {
        "semantic_sha256": semantic_hashes[0],
        "median_p50_ms": statistics.median(p50),
        "median_p95_ms": statistics.median(p95),
        "median_p99_ms": statistics.median(p99),
        "worst_maximum_ms": max(maximum),
        "median_wall_seconds": statistics.median(wall),
        "median_peak_pss_bytes": statistics.median(pss_values) if pss_values else None,
    }


def validate_absolute_gates(name: str, metrics: dict[str, Any], label: str) -> None:
    gates = {
        "unicode": (0.50, 0.75, float("inf")),
        "grapheme": (1.50, 2.00, 3.00),
        "script": (1.50, 2.00, 3.00),
        "bidi": (2.00, 3.00, 6.00),
    }
    p95_gate, p99_gate, maximum_gate = gates[name]
    require(metrics["median_p95_ms"] <= p95_gate, f"{label}.{name} median P95 exceeded gate")
    require(metrics["median_p99_ms"] <= p99_gate, f"{label}.{name} median P99 exceeded gate")
    require(metrics["worst_maximum_ms"] <= maximum_gate, f"{label}.{name} maximum exceeded gate")


def conformance_metrics(report: dict[str, Any], name: str, label: str) -> dict[str, Any]:
    measured = report["conformance"].get(name)
    require(isinstance(measured, dict), f"{label}.{name} conformance missing")
    result = result_of(measured, f"{label}.{name}.conformance")
    require(result.get("unicode_version") == "17.0.0", f"{label}.{name} Unicode version mismatch")
    require(result.get("passed") is True, f"{label}.{name} conformance failed")
    return {
        "semantic_sha256": canonical_sha256(result),
        "result": result,
        "wall_seconds": finite_non_negative(measured.get("seconds"), f"{label}.{name}.conformance.seconds"),
    }


def pss_ratio(shadow: int | float | None, baseline: int | float | None, label: str) -> float | None:
    if shadow is None or baseline is None:
        return None
    return positive_ratio(float(shadow), float(baseline), label)


def certify(
    baseline: dict[str, Any],
    shadow: dict[str, Any],
    *,
    commit_sha: str,
    runner_os: str,
    compiler: str,
    build_type: str,
    maximum_p50_ratio: float,
    maximum_p95_ratio: float,
    maximum_wall_ratio: float,
    maximum_peak_pss_ratio: float,
) -> dict[str, Any]:
    validate_report(baseline, "baseline")
    validate_report(shadow, "shadow")
    require(
        baseline["ucd_sources_sha256"] == shadow["ucd_sources_sha256"],
        "baseline and shadow used different Unicode source files",
    )

    baseline_probe = validate_probe(baseline, False, "baseline")
    shadow_probe = validate_probe(shadow, True, "shadow")
    require(
        baseline_probe["semantic_sha256"] == shadow_probe["semantic_sha256"],
        "text probe semantic output differs",
    )

    probe_wall_ratio = positive_ratio(
        shadow_probe["wall_seconds"], baseline_probe["wall_seconds"], "probe wall time"
    )
    probe_pss_ratio = pss_ratio(
        shadow_probe["peak_pss_bytes"], baseline_probe["peak_pss_bytes"], "probe peak PSS"
    )

    workloads: dict[str, Any] = {}
    all_gates = [probe_wall_ratio <= maximum_wall_ratio]
    if probe_pss_ratio is not None:
        all_gates.append(probe_pss_ratio <= maximum_peak_pss_ratio)

    for name in ("unicode", "grapheme", "script", "bidi"):
        baseline_metrics = benchmark_metrics(baseline, name, "baseline")
        shadow_metrics = benchmark_metrics(shadow, name, "shadow")
        validate_absolute_gates(name, baseline_metrics, "baseline")
        validate_absolute_gates(name, shadow_metrics, "shadow")
        require(
            baseline_metrics["semantic_sha256"] == shadow_metrics["semantic_sha256"],
            f"{name} benchmark semantic output differs",
        )
        p50_ratio_value = positive_ratio(
            shadow_metrics["median_p50_ms"], baseline_metrics["median_p50_ms"], f"{name} P50"
        )
        p95_ratio_value = positive_ratio(
            shadow_metrics["median_p95_ms"], baseline_metrics["median_p95_ms"], f"{name} P95"
        )
        wall_ratio_value = positive_ratio(
            shadow_metrics["median_wall_seconds"],
            baseline_metrics["median_wall_seconds"],
            f"{name} wall time",
        )
        peak_ratio_value = pss_ratio(
            shadow_metrics["median_peak_pss_bytes"],
            baseline_metrics["median_peak_pss_bytes"],
            f"{name} peak PSS",
        )
        workload_gates = {
            "p50_ratio": p50_ratio_value <= maximum_p50_ratio,
            "p95_ratio": p95_ratio_value <= maximum_p95_ratio,
            "wall_ratio": wall_ratio_value <= maximum_wall_ratio,
            "peak_pss_ratio": peak_ratio_value is None
            or peak_ratio_value <= maximum_peak_pss_ratio,
        }
        all_gates.extend(workload_gates.values())
        workloads[name] = {
            "semantic_sha256": baseline_metrics["semantic_sha256"],
            "baseline": baseline_metrics,
            "shadow": shadow_metrics,
            "ratios": {
                "median_p50": p50_ratio_value,
                "median_p95": p95_ratio_value,
                "median_wall": wall_ratio_value,
                "median_peak_pss": peak_ratio_value,
            },
            "gates": workload_gates,
        }

    conformance: dict[str, Any] = {}
    for name in ("grapheme", "script", "bidi"):
        baseline_conformance = conformance_metrics(baseline, name, "baseline")
        shadow_conformance = conformance_metrics(shadow, name, "shadow")
        require(
            baseline_conformance["semantic_sha256"]
            == shadow_conformance["semantic_sha256"],
            f"{name} conformance output differs",
        )
        conformance[name] = {
            "semantic_sha256": baseline_conformance["semantic_sha256"],
            "baseline_wall_seconds": baseline_conformance["wall_seconds"],
            "shadow_wall_seconds": shadow_conformance["wall_seconds"],
        }

    slice_ready = all(all_gates)
    manifest: dict[str, Any] = {
        "schema": "zevryon.rust-shadow-certification.v1",
        "slice": "Z2R-1D2-unicode-text",
        "build": {
            "commit_sha": commit_sha,
            "runner_os": runner_os,
            "compiler": compiler,
            "build_type": build_type,
        },
        "inputs": {
            "baseline_sha256": canonical_sha256(baseline),
            "shadow_sha256": canonical_sha256(shadow),
            "ucd_sources_sha256": baseline["ucd_sources_sha256"],
        },
        "probe": {
            "semantic_sha256": baseline_probe["semantic_sha256"],
            "pipeline_checksum": baseline_probe["pipeline_checksum"],
            "shadow_operations": shadow_probe["operations"],
            "shadow_verifications": shadow_probe["verifications"],
            "shadow_mismatches": shadow_probe["mismatches"],
            "wall_ratio": probe_wall_ratio,
            "peak_pss_ratio": probe_pss_ratio,
        },
        "workloads": workloads,
        "conformance": conformance,
        "gates": {
            "maximum_p50_ratio": maximum_p50_ratio,
            "maximum_p95_ratio": maximum_p95_ratio,
            "maximum_wall_ratio": maximum_wall_ratio,
            "maximum_peak_pss_ratio": maximum_peak_pss_ratio,
            "exact_semantic_parity": True,
            "exact_unicode_17_conformance": True,
            "zero_rust_mismatches": shadow_probe["mismatches"] == 0,
            "rollback_available": True,
        },
        "completed_slices": [
            "Z2R-1D1-massivedoc-zenith",
            "Z2R-1D2-unicode-text",
        ],
        "blocked_by": [
            "Z2R-1D3-font-discovery-fallback-shaping",
            "Z2R-1D4-windows-macos-workload-overhead",
        ],
        "rollback": {
            "ZEVRYON_ENABLE_RUST_CORE": "OFF",
            "ZEVRYON_RUST_LEDGER_SHADOW": "OFF",
        },
        "slice_ready": slice_ready,
        "promotion_ready": False,
    }
    manifest["manifest_sha256"] = canonical_sha256(manifest)
    require(slice_ready, "Z2R-1D2 workload overhead gate failed")
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description="Certify paired Unicode text Rust-shadow workloads")
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--shadow", type=Path, required=True)
    parser.add_argument("--commit-sha", required=True)
    parser.add_argument("--runner-os", required=True)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--build-type", default="Release")
    parser.add_argument("--maximum-p50-ratio", type=float, default=1.50)
    parser.add_argument("--maximum-p95-ratio", type=float, default=1.50)
    parser.add_argument("--maximum-wall-ratio", type=float, default=1.75)
    parser.add_argument("--maximum-peak-pss-ratio", type=float, default=1.50)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    for name in (
        "maximum_p50_ratio",
        "maximum_p95_ratio",
        "maximum_wall_ratio",
        "maximum_peak_pss_ratio",
    ):
        require(getattr(args, name) > 0.0, f"{name} must be positive")

    manifest = certify(
        load_json(args.baseline),
        load_json(args.shadow),
        commit_sha=args.commit_sha,
        runner_os=args.runner_os,
        compiler=args.compiler,
        build_type=args.build_type,
        maximum_p50_ratio=args.maximum_p50_ratio,
        maximum_p95_ratio=args.maximum_p95_ratio,
        maximum_wall_ratio=args.maximum_wall_ratio,
        maximum_peak_pss_ratio=args.maximum_peak_pss_ratio,
    )
    text = json.dumps(manifest, indent=2, sort_keys=True) + "\n"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(text, encoding="utf-8")
    print(text, end="")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except CertificationError as error:
        print(f"Z2R-1D2 certification failed: {error}")
        raise SystemExit(1) from error
