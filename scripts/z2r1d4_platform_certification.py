#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import sys
from typing import Any

RUN_SCHEMA = "zevryon.z2r1d4.platform-run.v1"
MANIFEST_SCHEMA = "zevryon.rust-shadow-certification.v1"
SLICE = "Z2R-1D4-platform-overhead"
EXPECTED_WORKLOADS = [
    "platform_discovery",
    "font_content_identity",
    "font_load_locator",
    "shaping_run_plan",
]
EXPECTED_ADAPTER = {"windows": "directwrite", "macos": "coretext"}


class CertificationError(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise CertificationError(message)


def load_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise CertificationError(f"cannot load {path}: {error}") from error
    if not isinstance(value, dict):
        raise CertificationError(f"{path} is not a JSON object")
    return value


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


def validate_probe(probe: Any) -> dict[str, Any]:
    require(isinstance(probe, dict), "shadow run is missing the exact Rust probe")
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
    operations = int(probe.get("rust_shadow_operations", 0))
    verifications = int(probe.get("rust_shadow_verifications", 0))
    require(operations > 0, "probe executed no operations")
    require(verifications > 0, "probe executed no verifications")
    return {
        "operations": operations,
        "verifications": verifications,
        "mismatches": 0,
        "trace_checksum": str(probe.get("trace_checksum", "")),
    }


def validate_run(
    report: dict[str, Any], mode: str, platform_name: str
) -> dict[str, dict[str, Any]]:
    require(report.get("schema") == RUN_SCHEMA, f"{mode} run schema mismatch")
    require(report.get("mode") == mode, f"{mode} mode mismatch")
    require(report.get("platform") == platform_name, f"{mode} platform mismatch")
    require(
        report.get("adapter") == EXPECTED_ADAPTER[platform_name],
        f"{mode} adapter mismatch",
    )
    tests = report.get("tests")
    require(isinstance(tests, dict), f"{mode}.tests missing")
    names = tests.get("names")
    require(isinstance(names, list) and len(names) == 9, f"{mode} test suite incomplete")
    require(int(tests.get("count", 0)) == len(names), f"{mode} test count mismatch")
    require(len(str(tests.get("names_sha256", ""))) == 64, f"{mode} test hash invalid")

    workloads = report.get("workloads")
    require(isinstance(workloads, list), f"{mode}.workloads missing")
    actual_names = [item.get("name") for item in workloads if isinstance(item, dict)]
    require(actual_names == EXPECTED_WORKLOADS, f"{mode} workload order/scope mismatch")
    indexed: dict[str, dict[str, Any]] = {}
    for item in workloads:
        require(isinstance(item, dict), f"{mode} workload is not an object")
        name = str(item["name"])
        require(len(str(item.get("semantic_sha256", ""))) == 64, f"{name} semantic SHA invalid")
        require(int(item.get("sample_count", 0)) >= 3, f"{name} has too few samples")
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
    *,
    platform_name: str,
    commit_sha: str,
    compiler: str,
    build_type: str,
    max_p50_ratio: float,
    max_p95_ratio: float,
    max_wall_ratio: float,
    max_peak_rss_ratio: float,
    max_peak_rss_delta_bytes: int,
) -> dict[str, Any]:
    require(platform_name in EXPECTED_ADAPTER, "unsupported platform")
    require(max_peak_rss_delta_bytes >= 0, "peak RSS delta gate must be non-negative")
    baseline_workloads = validate_run(baseline, "baseline", platform_name)
    shadow_workloads = validate_run(shadow, "shadow", platform_name)
    probe = validate_probe(shadow.get("rust_shadow_probe"))
    require(baseline.get("rust_shadow_probe") is None, "baseline unexpectedly contains Rust probe")
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
        require(
            base["semantic_sha256"] == other["semantic_sha256"],
            f"{name} semantic output diverged",
        )
        p50_ratio = positive_ratio(other["median_p50_ms"], base["median_p50_ms"], f"{name}.p50")
        p95_ratio = positive_ratio(other["median_p95_ms"], base["median_p95_ms"], f"{name}.p95")
        wall_ratio = positive_ratio(
            other["median_wall_seconds"], base["median_wall_seconds"], f"{name}.wall"
        )
        baseline_peak = base.get("median_peak_rss_bytes")
        shadow_peak = other.get("median_peak_rss_bytes")
        peak_ratio = optional_ratio(
            shadow_peak,
            baseline_peak,
            f"{name}.peak_rss",
        )
        peak_delta_bytes = None
        memory_passed = peak_ratio is None
        if peak_ratio is not None:
            baseline_peak_number = finite_non_negative(
                baseline_peak, f"{name}.peak_rss.baseline"
            )
            shadow_peak_number = finite_non_negative(
                shadow_peak, f"{name}.peak_rss.shadow"
            )
            peak_delta_bytes = int(shadow_peak_number - baseline_peak_number)
            memory_passed = (
                peak_ratio <= max_peak_rss_ratio
                or peak_delta_bytes <= max_peak_rss_delta_bytes
            )
        passed = (
            p50_ratio <= max_p50_ratio
            and p95_ratio <= max_p95_ratio
            and wall_ratio <= max_wall_ratio
            and memory_passed
        )
        require(passed, f"{name} exceeded a performance or memory gate")
        workload_results.append(
            {
                "name": name,
                "semantic_sha256": base["semantic_sha256"],
                "p50_ratio": p50_ratio,
                "p95_ratio": p95_ratio,
                "wall_time_ratio": wall_ratio,
                "peak_rss_ratio": peak_ratio,
                "peak_rss_delta_bytes": peak_delta_bytes,
                "peak_rss_gate": "ratio-or-absolute-delta",
                "memory_passed": memory_passed,
                "passed": True,
            }
        )

    manifest: dict[str, Any] = {
        "schema": MANIFEST_SCHEMA,
        "slice": SLICE,
        "platform": platform_name,
        "adapter": EXPECTED_ADAPTER[platform_name],
        "commit_sha": commit_sha,
        "environment": {
            "compiler": compiler,
            "build_type": build_type,
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
        "tests": {
            "count": baseline["tests"]["count"],
            "names_sha256": baseline["tests"]["names_sha256"],
        },
        "probe": probe,
        "gates": {
            "max_p50_ratio": max_p50_ratio,
            "max_p95_ratio": max_p95_ratio,
            "max_wall_time_ratio": max_wall_ratio,
            "max_peak_rss_ratio": max_peak_rss_ratio,
            "max_peak_rss_delta_bytes": max_peak_rss_delta_bytes,
            "peak_rss_gate": "ratio-or-absolute-delta",
        },
        "workloads": workload_results,
        "slice_ready": True,
        "promotion_ready": False,
        "remaining_platforms": [],
    }
    manifest["manifest_sha256"] = hashlib.sha256(canonical_bytes(manifest)).hexdigest()
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--shadow", type=Path, required=True)
    parser.add_argument("--platform", choices=("windows", "macos"), required=True)
    parser.add_argument("--commit-sha", required=True)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--build-type", default="Release")
    parser.add_argument("--max-p50-ratio", type=float, default=1.50)
    parser.add_argument("--max-p95-ratio", type=float, default=1.50)
    parser.add_argument("--max-wall-ratio", type=float, default=1.75)
    parser.add_argument("--max-peak-rss-ratio", type=float, default=1.50)
    parser.add_argument(
        "--max-peak-rss-delta-bytes", type=int, default=8 * 1024 * 1024
    )
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    try:
        manifest = build_manifest(
            load_json(args.baseline),
            load_json(args.shadow),
            platform_name=args.platform,
            commit_sha=args.commit_sha,
            compiler=args.compiler,
            build_type=args.build_type,
            max_p50_ratio=args.max_p50_ratio,
            max_p95_ratio=args.max_p95_ratio,
            max_wall_ratio=args.max_wall_ratio,
            max_peak_rss_ratio=args.max_peak_rss_ratio,
            max_peak_rss_delta_bytes=args.max_peak_rss_delta_bytes,
        )
    except CertificationError as error:
        print(f"Z2R-1D4 platform certification failed: {error}", file=sys.stderr)
        return 1

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(
        json.dumps(
            {
                "schema": manifest["schema"],
                "slice": manifest["slice"],
                "platform": manifest["platform"],
                "slice_ready": manifest["slice_ready"],
                "manifest_sha256": manifest["manifest_sha256"],
            },
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
