#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
from typing import Any


FAULT_EXPECTATIONS = {
    "output": "output_record",
    "error": "error_kind",
    "state": "statistics",
    "reset": "reset_result",
}


def canonical_json(value: Any) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False)


def manifest_sha256(value: dict[str, Any]) -> str:
    payload = dict(value)
    payload.pop("manifest_sha256", None)
    return hashlib.sha256(canonical_json(payload).encode("utf-8")).hexdigest()


def report_sha256(value: dict[str, Any]) -> str:
    payload = dict(value)
    payload.pop("report_sha256", None)
    return hashlib.sha256(canonical_json(payload).encode("utf-8")).hexdigest()


def ratio(numerator: float, denominator: float) -> float:
    if denominator <= 0.0:
        if numerator <= 0.0:
            return 1.0
        raise ValueError("baseline metric is not positive")
    return numerator / denominator


def validate_faults(
    report: dict[str, Any],
    semantic_sha256: str,
) -> list[str]:
    faults = report.get("faults", {})
    if report.get("platform") != "linux":
        if faults:
            raise ValueError("fault evidence is only permitted on Linux")
        return []

    if set(faults) != set(FAULT_EXPECTATIONS):
        raise ValueError("Linux report must contain all four Unicode fault classes")

    certified: list[str] = []
    for name, expected_mismatch in FAULT_EXPECTATIONS.items():
        sample = faults[name]
        result = sample.get("result", {})
        shadow = result.get("shadow", {})
        if sample.get("semantic_sha256") != semantic_sha256:
            raise ValueError(f"{name} fault changed C++ authoritative semantics")
        if not shadow.get("enabled"):
            raise ValueError(f"{name} fault did not enable the Rust shadow")
        if shadow.get("healthy"):
            raise ValueError(f"{name} fault remained healthy")
        if int(shadow.get("mismatches", 0)) <= 0:
            raise ValueError(f"{name} fault was not detected")
        if shadow.get("first_mismatch") != expected_mismatch:
            raise ValueError(
                f"{name} fault latched {shadow.get('first_mismatch')!r}, "
                f"expected {expected_mismatch!r}"
            )
        certified.append(name)
    return certified


def certify_report(
    report: dict[str, Any],
    *,
    commit_sha: str,
    compiler: str,
    build_type: str,
    max_p50_ratio: float,
    max_p95_ratio: float,
    max_p99_ratio: float,
    max_maximum_ratio: float,
    max_memory_ratio: float,
    max_memory_delta_bytes: int,
) -> dict[str, Any]:
    if report.get("schema") != "zevryon.z2r3du.paired-report.v1":
        raise ValueError("unexpected paired report schema")
    if report.get("report_sha256") != report_sha256(report):
        raise ValueError("paired report SHA-256 mismatch")
    platform = report.get("platform")
    if platform not in {"linux", "windows", "macos"}:
        raise ValueError("invalid platform")
    if int(report.get("samples", 0)) < 3:
        raise ValueError("at least three paired samples are required")
    if int(report.get("logical_bytes", 0)) < 16 * 1024 * 1024:
        raise ValueError("certification corpus is smaller than 16 MiB")
    if int(report.get("rounds", 0)) < 2:
        raise ValueError("at least two workload rounds are required")

    baseline = report.get("baseline", [])
    shadow = report.get("shadow", [])
    if len(baseline) != int(report["samples"]) or len(shadow) != int(report["samples"]):
        raise ValueError("sample count mismatch")

    semantic_sha256 = str(report.get("semantic_sha256", ""))
    if len(semantic_sha256) != 64:
        raise ValueError("missing semantic SHA-256")
    for sample in baseline + shadow:
        if sample.get("semantic_sha256") != semantic_sha256:
            raise ValueError("semantic hash changed across samples")

    for sample in baseline:
        telemetry = sample.get("result", {}).get("shadow", {})
        if telemetry.get("enabled"):
            raise ValueError("baseline unexpectedly enabled Rust shadow")
    for sample in shadow:
        telemetry = sample.get("result", {}).get("shadow", {})
        if not telemetry.get("enabled"):
            raise ValueError("shadow sample did not enable Rust")
        if not telemetry.get("healthy"):
            raise ValueError("shadow sample reported unhealthy telemetry")
        if int(telemetry.get("mismatches", 0)) != 0:
            raise ValueError("positive shadow sample reported mismatch")
        if int(telemetry.get("operations", 0)) <= 0:
            raise ValueError("positive shadow sample reported no operations")
        if int(telemetry.get("verifications", 0)) <= 0:
            raise ValueError("positive shadow sample reported no verifications")

    seconds = report["seconds"]
    memory = report["peak_rss_bytes"]
    ratios = {
        "p50": ratio(float(seconds["shadow"]["p50"]), float(seconds["baseline"]["p50"])),
        "p95": ratio(float(seconds["shadow"]["p95"]), float(seconds["baseline"]["p95"])),
        "p99": ratio(float(seconds["shadow"]["p99"]), float(seconds["baseline"]["p99"])),
        "maximum": ratio(
            float(seconds["shadow"]["maximum"]),
            float(seconds["baseline"]["maximum"]),
        ),
        "peak_rss": ratio(
            float(memory["shadow"]["p50"]),
            float(memory["baseline"]["p50"]),
        ),
    }
    memory_delta = int(memory["shadow"]["p50"] - memory["baseline"]["p50"])

    if ratios["p50"] > max_p50_ratio:
        raise ValueError("p50 ratio exceeded gate")
    if ratios["p95"] > max_p95_ratio:
        raise ValueError("p95 ratio exceeded gate")
    if ratios["p99"] > max_p99_ratio:
        raise ValueError("p99 ratio exceeded gate")
    if ratios["maximum"] > max_maximum_ratio:
        raise ValueError("maximum ratio exceeded gate")
    if ratios["peak_rss"] > max_memory_ratio and memory_delta > max_memory_delta_bytes:
        raise ValueError("peak RSS exceeded both ratio and absolute-delta gates")

    faults = validate_faults(report, semantic_sha256)
    baseline_result = baseline[0]["result"]
    required_counts = {
        "malformed_cases": 7,
        "discontinuity_cases": 1,
        "budget_cases": 1,
    }
    for field, minimum in required_counts.items():
        if int(baseline_result.get(field, 0)) < minimum:
            raise ValueError(f"insufficient {field} coverage")
    if int(baseline_result.get("strict_failures", 0)) < 7:
        raise ValueError("strict malformed-stream failures were not covered")
    if int(baseline_result.get("replacement_records", 0)) < 7:
        raise ValueError("replacement-policy coverage is incomplete")

    manifest: dict[str, Any] = {
        "schema": "zevryon.z2r3du.platform-certification.v1",
        "slice": "Z2R-3D-U",
        "platform": platform,
        "commit_sha": commit_sha,
        "compiler": compiler,
        "build_type": build_type,
        "logical_bytes": int(report["logical_bytes"]),
        "rounds": int(report["rounds"]),
        "samples": int(report["samples"]),
        "semantic_sha256": semantic_sha256,
        "report_sha256": report.get("report_sha256"),
        "fault_classes": faults,
        "ratios": ratios,
        "peak_rss_delta_bytes": memory_delta,
        "authority": {
            "current": "cpp",
            "candidate": "rust",
            "switch_performed": False,
            "rollback_retained": True,
        },
        "platform_ready": True,
    }
    manifest["manifest_sha256"] = manifest_sha256(manifest)
    return manifest


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--report", required=True)
    parser.add_argument("--commit-sha", required=True)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--build-type", default="Release")
    parser.add_argument("--max-p50-ratio", type=float, default=2.00)
    parser.add_argument("--max-p95-ratio", type=float, default=2.25)
    parser.add_argument("--max-p99-ratio", type=float, default=2.50)
    parser.add_argument("--max-maximum-ratio", type=float, default=3.00)
    parser.add_argument("--max-memory-ratio", type=float, default=1.50)
    parser.add_argument("--max-memory-delta-bytes", type=int, default=16 * 1024 * 1024)
    parser.add_argument("--output", required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    report = json.loads(Path(args.report).read_text(encoding="utf-8"))
    manifest = certify_report(
        report,
        commit_sha=args.commit_sha,
        compiler=args.compiler,
        build_type=args.build_type,
        max_p50_ratio=args.max_p50_ratio,
        max_p95_ratio=args.max_p95_ratio,
        max_p99_ratio=args.max_p99_ratio,
        max_maximum_ratio=args.max_maximum_ratio,
        max_memory_ratio=args.max_memory_ratio,
        max_memory_delta_bytes=args.max_memory_delta_bytes,
    )
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(json.dumps({
        "platform": manifest["platform"],
        "platform_ready": manifest["platform_ready"],
        "manifest_sha256": manifest["manifest_sha256"],
        "ratios": manifest["ratios"],
        "fault_classes": manifest["fault_classes"],
    }, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
