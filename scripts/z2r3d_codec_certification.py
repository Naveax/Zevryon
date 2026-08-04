#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import sys
from typing import Any

RUN_SCHEMA = "zevryon.z2r3d.paired-run.v1"
MANIFEST_SCHEMA = "zevryon.z2r3d.platform-certification.v1"
OPERATIONS = ("import", "open", "verify", "export")
FAULTS = {
    "record-encode": "RecordEncode",
    "record-decode": "RecordDecode",
    "chunk-encode": "ChunkEncode",
    "chunk-decode": "ChunkDecode",
}


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
    require(isinstance(value, dict), f"{path} is not a JSON object")
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
    require(math.isfinite(number) and number >= 0.0, f"{label} must be finite and non-negative")
    return number


def ratio_and_delta(shadow: Any, baseline: Any, label: str) -> tuple[float, float]:
    base = finite_non_negative(baseline, f"{label}.baseline")
    other = finite_non_negative(shadow, f"{label}.shadow")
    require(base > 0.0, f"{label} baseline must be positive")
    return other / base, other - base


def metric_gate(
    shadow: Any,
    baseline: Any,
    *,
    label: str,
    max_ratio: float,
    max_delta: float,
) -> dict[str, Any]:
    ratio, delta = ratio_and_delta(shadow, baseline, label)
    passed = ratio <= max_ratio or delta <= max_delta
    require(passed, f"{label} exceeded ratio and absolute-delta gates")
    return {
        "baseline": float(baseline),
        "shadow": float(shadow),
        "ratio": ratio,
        "delta": delta,
        "max_ratio": max_ratio,
        "max_delta": max_delta,
        "gate": "ratio-or-absolute-delta",
        "passed": True,
    }


def expected_telemetry(operation: str, records: int, chunks: int) -> dict[str, int]:
    if operation == "import":
        return {
            "record_encode_checks": records,
            "record_decode_checks": 0,
            "chunk_encode_checks": chunks,
            "chunk_decode_checks": 0,
        }
    if operation == "open":
        return {
            "record_encode_checks": 0,
            "record_decode_checks": 0,
            "chunk_encode_checks": 0,
            "chunk_decode_checks": 0,
        }
    if operation == "verify":
        return {
            "record_encode_checks": 0,
            "record_decode_checks": records * 2,
            "chunk_encode_checks": 0,
            "chunk_decode_checks": chunks,
        }
    if operation == "export":
        return {
            "record_encode_checks": 0,
            "record_decode_checks": records,
            "chunk_encode_checks": 0,
            "chunk_decode_checks": chunks,
        }
    raise CertificationError(f"unknown operation {operation}")


def validate_report(report: dict[str, Any], platform_name: str) -> dict[str, Any]:
    require(report.get("schema") == RUN_SCHEMA, "paired-run schema mismatch")
    require(report.get("platform") == platform_name, "platform mismatch")
    require(report.get("exact_store_tree_parity") is True, "store tree parity failed")
    require(report.get("exact_export_parity") is True, "export parity failed")
    require(report.get("zero_shadow_mismatches") is True, "shadow mismatch flag is false")
    require(report.get("all_fault_classes_detected") is True, "fault coverage flag is false")
    require(len(str(report.get("payload_sha256", ""))) == 64, "payload SHA-256 invalid")
    require(len(str(report.get("report_sha256", ""))) == 64, "report SHA-256 invalid")

    parameters = report.get("parameters")
    require(isinstance(parameters, dict), "parameters missing")
    logical_bytes = int(parameters.get("logical_bytes", 0))
    records = int(parameters.get("records", 0))
    segment_bytes = int(parameters.get("segment_bytes", 0))
    giant_record_bytes = int(parameters.get("giant_record_bytes", 0))
    samples = int(parameters.get("samples", 0))
    require(logical_bytes >= 128 * 1024 * 1024, "corpus is smaller than 128 MiB")
    require(records >= 100_000, "record count is below 100,000")
    require(segment_bytes > 0, "segment size is invalid")
    require(giant_record_bytes >= 64 * 1024 * 1024, "64 MiB giant record missing")
    require(samples >= 3, "fewer than three samples")

    canonical_store = report.get("canonical_store")
    require(isinstance(canonical_store, dict), "canonical store manifest missing")
    require(int(canonical_store.get("file_count", 0)) > 0, "canonical store is empty")
    require(len(str(canonical_store.get("tree_sha256", ""))) == 64, "store tree SHA invalid")

    import_pairs = report.get("import_pairs")
    require(isinstance(import_pairs, list) and len(import_pairs) == samples, "import pair count mismatch")
    tree_hashes = {str(item.get("store_tree_sha256", "")) for item in import_pairs}
    require(tree_hashes == {canonical_store["tree_sha256"]}, "store tree changed between samples")

    export_pairs = report.get("export_pairs")
    require(isinstance(export_pairs, list) and len(export_pairs) == samples, "export pair count mismatch")
    for item in export_pairs:
        require(int(item.get("bytes", -1)) == logical_bytes, "exported payload size mismatch")
        require(item.get("sha256") == report["payload_sha256"], "exported payload SHA mismatch")

    faults = report.get("faults")
    require(isinstance(faults, dict) and set(faults) == set(FAULTS), "fault class set mismatch")
    for name, mismatch in FAULTS.items():
        item = faults[name]
        require(item.get("expected_first_mismatch") == mismatch, f"{name} expected class mismatch")
        telemetry = item.get("telemetry")
        require(isinstance(telemetry, dict), f"{name} telemetry missing")
        require(int(telemetry.get("mismatches", 0)) == 1, f"{name} mismatch count invalid")
        require(telemetry.get("first_mismatch") == mismatch, f"{name} latch mismatch")

    operations = report.get("operations")
    require(isinstance(operations, dict) and set(operations) == set(OPERATIONS), "operation set mismatch")
    for operation in OPERATIONS:
        item = operations[operation]
        require(len(str(item.get("semantic_sha256", ""))) == 64, f"{operation} semantic SHA invalid")
        telemetry_samples = item.get("shadow_telemetry")
        require(
            isinstance(telemetry_samples, list) and len(telemetry_samples) == samples,
            f"{operation} telemetry sample count mismatch",
        )
        expected = expected_telemetry(operation, records, int(telemetry_samples[0].get("chunk_decode_checks", 0)))
        if operation in ("import", "open"):
            first_store_chunks = None
            if operation == "import":
                first_store_chunks = int(telemetry_samples[0]["chunk_encode_checks"])
            else:
                first_store_chunks = int(report["operations"]["verify"]["shadow_telemetry"][0]["chunk_decode_checks"])
            expected = expected_telemetry(operation, records, first_store_chunks)
        else:
            chunks = int(telemetry_samples[0]["chunk_decode_checks"])
            expected = expected_telemetry(operation, records, chunks)
        for telemetry in telemetry_samples:
            require(telemetry.get("enabled") is True, f"{operation} shadow disabled")
            require(int(telemetry.get("mismatches", -1)) == 0, f"{operation} mismatch recorded")
            require(telemetry.get("first_mismatch") == "None", f"{operation} mismatch latched")
            for key, value in expected.items():
                require(int(telemetry.get(key, -1)) == value, f"{operation} {key} mismatch")
        for mode in ("baseline", "shadow"):
            summary = item.get(mode)
            require(isinstance(summary, dict), f"{operation}.{mode} summary missing")
            require(int(summary.get("sample_count", 0)) == samples, f"{operation}.{mode} samples mismatch")
            for family in ("wall_seconds", "internal_seconds"):
                metrics = summary.get(family)
                require(isinstance(metrics, dict), f"{operation}.{mode}.{family} missing")
                for name in ("p50", "p95", "p99", "maximum"):
                    finite_non_negative(metrics.get(name), f"{operation}.{mode}.{family}.{name}")
            for family in ("peak_rss_bytes",):
                metrics = summary.get(family)
                require(isinstance(metrics, dict), f"{operation}.{mode}.{family} missing")
                for name in ("p50", "maximum"):
                    finite_non_negative(metrics.get(name), f"{operation}.{mode}.{family}.{name}")
    return {
        "logical_bytes": logical_bytes,
        "records": records,
        "segment_bytes": segment_bytes,
        "giant_record_bytes": giant_record_bytes,
        "samples": samples,
    }


def build_manifest(
    report: dict[str, Any],
    *,
    platform_name: str,
    commit_sha: str,
    compiler: str,
    build_type: str,
    max_p50_ratio: float,
    max_p95_ratio: float,
    max_p99_ratio: float,
    max_maximum_ratio: float,
    max_wall_delta_seconds: float,
    max_memory_ratio: float,
    max_memory_delta_bytes: int,
    max_total_ratio: float,
    max_total_delta_seconds: float,
) -> dict[str, Any]:
    parameters = validate_report(report, platform_name)
    operation_results: list[dict[str, Any]] = []
    total_baseline = 0.0
    total_shadow = 0.0

    for operation in OPERATIONS:
        item = report["operations"][operation]
        baseline = item["baseline"]["wall_seconds"]
        shadow = item["shadow"]["wall_seconds"]
        performance = {
            "p50": metric_gate(
                shadow["p50"], baseline["p50"], label=f"{operation}.p50",
                max_ratio=max_p50_ratio, max_delta=max_wall_delta_seconds,
            ),
            "p95": metric_gate(
                shadow["p95"], baseline["p95"], label=f"{operation}.p95",
                max_ratio=max_p95_ratio, max_delta=max_wall_delta_seconds,
            ),
            "p99": metric_gate(
                shadow["p99"], baseline["p99"], label=f"{operation}.p99",
                max_ratio=max_p99_ratio, max_delta=max_wall_delta_seconds,
            ),
            "maximum": metric_gate(
                shadow["maximum"], baseline["maximum"], label=f"{operation}.maximum",
                max_ratio=max_maximum_ratio, max_delta=max_wall_delta_seconds,
            ),
        }
        total_baseline += float(baseline["p50"])
        total_shadow += float(shadow["p50"])

        use_pss = platform_name == "linux"
        memory_name = "peak_pss_bytes" if use_pss else "peak_rss_bytes"
        baseline_memory = item["baseline"].get(memory_name)
        shadow_memory = item["shadow"].get(memory_name)
        require(isinstance(baseline_memory, dict), f"{operation} baseline {memory_name} missing")
        require(isinstance(shadow_memory, dict), f"{operation} shadow {memory_name} missing")
        memory = {
            "metric": memory_name,
            "p50": metric_gate(
                shadow_memory["p50"], baseline_memory["p50"], label=f"{operation}.{memory_name}.p50",
                max_ratio=max_memory_ratio, max_delta=float(max_memory_delta_bytes),
            ),
            "maximum": metric_gate(
                shadow_memory["maximum"], baseline_memory["maximum"],
                label=f"{operation}.{memory_name}.maximum",
                max_ratio=max_memory_ratio, max_delta=float(max_memory_delta_bytes),
            ),
        }
        operation_results.append(
            {
                "operation": operation,
                "semantic_sha256": item["semantic_sha256"],
                "performance": performance,
                "memory": memory,
                "passed": True,
            }
        )

    total = metric_gate(
        total_shadow,
        total_baseline,
        label="total_p50_wall_seconds",
        max_ratio=max_total_ratio,
        max_delta=max_total_delta_seconds,
    )
    manifest: dict[str, Any] = {
        "schema": MANIFEST_SCHEMA,
        "slice": "Z2R-3D-massivedoc-codec-promotion",
        "platform": platform_name,
        "commit_sha": commit_sha,
        "environment": {"compiler": compiler, "build_type": build_type},
        "parameters": parameters,
        "authority": {
            "authoritative_backend": "cpp",
            "shadow_backend": "rust",
            "authoritative_switch_performed": False,
            "rollback": [
                "ZEVRYON_ENABLE_RUST_CORE=OFF",
                "ZEVRYON_RUST_MASSIVEDOC_CODEC_SHADOW=OFF",
            ],
        },
        "parity": {
            "payload_sha256": report["payload_sha256"],
            "store_tree_sha256": report["canonical_store"]["tree_sha256"],
            "store_file_count": report["canonical_store"]["file_count"],
            "exact_store_tree": True,
            "exact_export": True,
            "zero_mismatches": True,
            "fault_classes": FAULTS,
        },
        "gates": {
            "max_p50_ratio": max_p50_ratio,
            "max_p95_ratio": max_p95_ratio,
            "max_p99_ratio": max_p99_ratio,
            "max_maximum_ratio": max_maximum_ratio,
            "max_wall_delta_seconds": max_wall_delta_seconds,
            "max_memory_ratio": max_memory_ratio,
            "max_memory_delta_bytes": max_memory_delta_bytes,
            "max_total_ratio": max_total_ratio,
            "max_total_delta_seconds": max_total_delta_seconds,
            "gate_policy": "ratio-or-absolute-delta",
        },
        "operations": operation_results,
        "total_p50_wall_seconds": total,
        "source_report_sha256": report["report_sha256"],
        "slice_ready": True,
        "promotion_ready": False,
    }
    manifest["manifest_sha256"] = hashlib.sha256(canonical_bytes(manifest)).hexdigest()
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--platform", choices=("linux", "windows", "macos"), required=True)
    parser.add_argument("--commit-sha", required=True)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--build-type", default="Release")
    parser.add_argument("--max-p50-ratio", type=float, default=2.00)
    parser.add_argument("--max-p95-ratio", type=float, default=2.25)
    parser.add_argument("--max-p99-ratio", type=float, default=2.50)
    parser.add_argument("--max-maximum-ratio", type=float, default=3.00)
    parser.add_argument("--max-wall-delta-seconds", type=float, default=5.0)
    parser.add_argument("--max-memory-ratio", type=float, default=1.50)
    parser.add_argument("--max-memory-delta-bytes", type=int, default=16 * 1024 * 1024)
    parser.add_argument("--max-total-ratio", type=float, default=2.00)
    parser.add_argument("--max-total-delta-seconds", type=float, default=10.0)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    try:
        manifest = build_manifest(
            load_json(args.report),
            platform_name=args.platform,
            commit_sha=args.commit_sha,
            compiler=args.compiler,
            build_type=args.build_type,
            max_p50_ratio=args.max_p50_ratio,
            max_p95_ratio=args.max_p95_ratio,
            max_p99_ratio=args.max_p99_ratio,
            max_maximum_ratio=args.max_maximum_ratio,
            max_wall_delta_seconds=args.max_wall_delta_seconds,
            max_memory_ratio=args.max_memory_ratio,
            max_memory_delta_bytes=args.max_memory_delta_bytes,
            max_total_ratio=args.max_total_ratio,
            max_total_delta_seconds=args.max_total_delta_seconds,
        )
    except CertificationError as error:
        print(f"Z2R-3D platform certification failed: {error}", file=sys.stderr)
        return 1

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(
        json.dumps(
            {
                "schema": manifest["schema"],
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
