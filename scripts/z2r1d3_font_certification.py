#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import sys
from typing import Any

RUN_SCHEMA = "zevryon.z2r1d3.font-shaping-run.v1"
MANIFEST_SCHEMA = "zevryon.rust-shadow-certification.v1"
EXPECTED_WORKLOADS = [
    "font_content_identity",
    "font_load_locator",
    "shaping_run_plan",
    "harfbuzz_uncached",
    "harfbuzz_verified_resource",
    "prepared_harfbuzz",
    "cached_catalog_harfbuzz",
    "multi_run_harfbuzz",
    "glyph_cluster_map",
    "caret_boundary_map",
]


class CertificationError(RuntimeError):
    pass


def load_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise CertificationError(f"cannot load {path}: {error}") from error
    if not isinstance(value, dict):
        raise CertificationError(f"{path} is not a JSON object")
    return value


def require(condition: bool, message: str) -> None:
    if not condition:
        raise CertificationError(message)


def canonical_bytes(value: Any) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode(
        "utf-8"
    )


def finite_non_negative(value: Any, label: str) -> float:
    try:
        number = float(value)
    except (TypeError, ValueError) as error:
        raise CertificationError(f"{label} is not numeric") from error
    if not math.isfinite(number) or number < 0.0:
        raise CertificationError(f"{label} must be finite and non-negative")
    return number


def positive_ratio(shadow: Any, baseline: Any, label: str) -> float:
    base = finite_non_negative(baseline, f"{label}.baseline")
    other = finite_non_negative(shadow, f"{label}.shadow")
    if base <= 0.0:
        raise CertificationError(f"{label} baseline must be positive")
    return other / base


def optional_ratio(shadow: Any, baseline: Any, label: str) -> float | None:
    if shadow is None or baseline is None:
        return None
    return positive_ratio(shadow, baseline, label)


def validate_probe(probe: dict[str, Any]) -> dict[str, Any]:
    require(
        probe.get("schema") == "zevryon.rust-shadow-workload-probe.v1",
        "Rust shadow probe schema mismatch",
    )
    require(probe.get("resource_class_count") == 36, "probe did not cover 36 classes")
    require(probe.get("rust_shadow_enabled") is True, "probe Rust shadow is disabled")
    require(probe.get("rust_shadow_healthy") is True, "probe Rust shadow is unhealthy")
    require(probe.get("rust_shadow_mismatches") == 0, "probe recorded a mismatch")
    require(probe.get("total_current_bytes") == 0, "probe leaked current bytes")
    require(probe.get("within_hard_limits") is True, "probe exceeded hard limits")
    require(probe.get("accounting_clean") is True, "probe accounting is dirty")
    require(int(probe.get("rust_shadow_operations", 0)) > 0, "probe executed no operations")
    require(int(probe.get("rust_shadow_verifications", 0)) > 0, "probe executed no verification")
    return {
        "operations": int(probe["rust_shadow_operations"]),
        "verifications": int(probe["rust_shadow_verifications"]),
        "mismatches": int(probe["rust_shadow_mismatches"]),
        "trace_checksum": str(probe.get("trace_checksum", "")),
    }


def validate_run(report: dict[str, Any], mode: str) -> dict[str, dict[str, Any]]:
    require(report.get("schema") == RUN_SCHEMA, f"{mode} run schema mismatch")
    require(report.get("mode") == mode, f"{mode} mode mismatch")
    fonts = report.get("fonts")
    require(isinstance(fonts, dict), f"{mode}.fonts missing")
    for name in ("latin", "devanagari"):
        item = fonts.get(name)
        require(isinstance(item, dict), f"{mode}.fonts.{name} missing")
        require(int(item.get("bytes", 0)) > 0, f"{mode}.{name} font is empty")
        require(len(str(item.get("sha256", ""))) == 64, f"{mode}.{name} SHA invalid")

    tests = report.get("tests")
    require(isinstance(tests, dict), f"{mode}.tests missing")
    names = tests.get("names")
    require(isinstance(names, list) and len(names) >= 20, f"{mode} test suite incomplete")
    require(int(tests.get("count", 0)) == len(names), f"{mode} test count mismatch")

    workloads = report.get("workloads")
    require(isinstance(workloads, list), f"{mode}.workloads missing")
    actual_names = [item.get("name") for item in workloads if isinstance(item, dict)]
    require(actual_names == EXPECTED_WORKLOADS, f"{mode} workload order/scope mismatch")
    indexed: dict[str, dict[str, Any]] = {}
    for item in workloads:
        require(isinstance(item, dict), f"{mode} workload is not an object")
        name = str(item["name"])
        require(len(str(item.get("semantic_sha256", ""))) == 64, f"{name} semantic SHA invalid")
        require(int(item.get("sample_count", 0)) >= 1, f"{name} has no samples")
        finite_non_negative(item.get("median_p50_ms"), f"{mode}.{name}.p50")
        finite_non_negative(item.get("median_p95_ms"), f"{mode}.{name}.p95")
        finite_non_negative(item.get("median_wall_seconds"), f"{mode}.{name}.wall")
        require(item.get("accounting_clean") is True, f"{mode}.{name} accounting is dirty")
        require(item.get("within_hard_limits") is True, f"{mode}.{name} exceeded hard limits")
        indexed[name] = item
    return indexed


def build_manifest(
    baseline: dict[str, Any],
    shadow: dict[str, Any],
    probe: dict[str, Any],
    *,
    commit_sha: str,
    compiler: str,
    build_type: str,
    runner_os: str,
    max_p50_ratio: float,
    max_p95_ratio: float,
    max_wall_ratio: float,
    max_peak_pss_ratio: float,
) -> dict[str, Any]:
    baseline_workloads = validate_run(baseline, "baseline")
    shadow_workloads = validate_run(shadow, "shadow")
    probe_summary = validate_probe(probe)

    require(baseline["fonts"] == shadow["fonts"], "baseline/shadow font fixtures differ")
    require(
        baseline["tests"]["names"] == shadow["tests"]["names"],
        "baseline/shadow test inventories differ",
    )
    require(
        baseline["tests"]["names_sha256"] == shadow["tests"]["names_sha256"],
        "baseline/shadow test inventory hashes differ",
    )

    workload_results: list[dict[str, Any]] = []
    for name in EXPECTED_WORKLOADS:
        base = baseline_workloads[name]
        other = shadow_workloads[name]
        semantic_equal = base["semantic_sha256"] == other["semantic_sha256"]
        require(semantic_equal, f"{name} semantic output diverged")
        p50_ratio = positive_ratio(other["median_p50_ms"], base["median_p50_ms"], f"{name}.p50")
        p95_ratio = positive_ratio(other["median_p95_ms"], base["median_p95_ms"], f"{name}.p95")
        wall_ratio = positive_ratio(
            other["median_wall_seconds"], base["median_wall_seconds"], f"{name}.wall"
        )
        peak_ratio = optional_ratio(
            other.get("median_peak_pss_bytes"),
            base.get("median_peak_pss_bytes"),
            f"{name}.peak_pss",
        )
        passed = (
            p50_ratio <= max_p50_ratio
            and p95_ratio <= max_p95_ratio
            and wall_ratio <= max_wall_ratio
            and (peak_ratio is None or peak_ratio <= max_peak_pss_ratio)
        )
        require(passed, f"{name} exceeded a performance or memory gate")
        workload_results.append(
            {
                "name": name,
                "semantic_sha256": base["semantic_sha256"],
                "p50_ratio": p50_ratio,
                "p95_ratio": p95_ratio,
                "wall_time_ratio": wall_ratio,
                "peak_pss_ratio": peak_ratio,
                "passed": passed,
            }
        )

    manifest: dict[str, Any] = {
        "schema": MANIFEST_SCHEMA,
        "slice": "Z2R-1D3-font-discovery-fallback-shaping",
        "commit_sha": commit_sha,
        "environment": {
            "compiler": compiler,
            "build_type": build_type,
            "runner_os": runner_os,
        },
        "authority": {
            "authoritative_backend": "cpp",
            "shadow_backend": "rust",
            "rust_authoritative": False,
            "rollback": [
                "ZEVRYON_ENABLE_RUST_CORE=OFF",
                "ZEVRYON_RUST_LEDGER_SHADOW=OFF",
            ],
        },
        "font_fixtures": baseline["fonts"],
        "tests": {
            "count": baseline["tests"]["count"],
            "names_sha256": baseline["tests"]["names_sha256"],
        },
        "probe": probe_summary,
        "gates": {
            "max_p50_ratio": max_p50_ratio,
            "max_p95_ratio": max_p95_ratio,
            "max_wall_time_ratio": max_wall_ratio,
            "max_peak_pss_ratio": max_peak_pss_ratio,
        },
        "workloads": workload_results,
        "slice_ready": True,
        "promotion_ready": False,
        "remaining_slices": ["Z2R-1D4-windows-workload-overhead"],
    }
    manifest["manifest_sha256"] = hashlib.sha256(canonical_bytes(manifest)).hexdigest()
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--shadow", type=Path, required=True)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--commit-sha", required=True)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--build-type", required=True)
    parser.add_argument("--runner-os", required=True)
    parser.add_argument("--max-p50-ratio", type=float, default=1.50)
    parser.add_argument("--max-p95-ratio", type=float, default=1.50)
    parser.add_argument("--max-wall-ratio", type=float, default=1.75)
    parser.add_argument("--max-peak-pss-ratio", type=float, default=1.50)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    try:
        manifest = build_manifest(
            load_json(args.baseline),
            load_json(args.shadow),
            load_json(args.probe),
            commit_sha=args.commit_sha,
            compiler=args.compiler,
            build_type=args.build_type,
            runner_os=args.runner_os,
            max_p50_ratio=args.max_p50_ratio,
            max_p95_ratio=args.max_p95_ratio,
            max_wall_ratio=args.max_wall_ratio,
            max_peak_pss_ratio=args.max_peak_pss_ratio,
        )
    except CertificationError as error:
        print(f"Z2R-1D3 certification failed: {error}", file=sys.stderr)
        return 1

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps({
        "schema": manifest["schema"],
        "slice": manifest["slice"],
        "slice_ready": manifest["slice_ready"],
        "promotion_ready": manifest["promotion_ready"],
        "manifest_sha256": manifest["manifest_sha256"],
    }, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
