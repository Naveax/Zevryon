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
COUNTERS = (
    "record_encode_checks",
    "record_decode_checks",
    "chunk_encode_checks",
    "chunk_decode_checks",
)


class CertificationError(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise CertificationError(message)


def canonical_bytes(value: Any) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode(
        "utf-8"
    )


def load_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise CertificationError(f"cannot load {path}: {error}") from error
    require(isinstance(value, dict), f"{path} is not a JSON object")
    return value


def finite_non_negative(value: Any, label: str) -> float:
    try:
        number = float(value)
    except (TypeError, ValueError) as error:
        raise CertificationError(f"{label} is not numeric") from error
    require(math.isfinite(number) and number >= 0.0, f"{label} must be finite and non-negative")
    return number


def metric_gate(
    shadow: Any,
    baseline: Any,
    *,
    label: str,
    max_ratio: float,
    max_delta: float,
) -> dict[str, Any]:
    base = finite_non_negative(baseline, f"{label}.baseline")
    other = finite_non_negative(shadow, f"{label}.shadow")
    require(base > 0.0, f"{label} baseline must be positive")
    ratio = other / base
    delta = other - base
    require(
        ratio <= max_ratio or delta <= max_delta,
        f"{label} exceeded ratio and absolute-delta gates",
    )
    return {
        "baseline": base,
        "shadow": other,
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
        return {counter: 0 for counter in COUNTERS}
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


def validate_summary(summary: Any, label: str, samples: int, platform_name: str) -> None:
    require(isinstance(summary, dict), f"{label} summary missing")
    require(int(summary.get("sample_count", 0)) == samples, f"{label} sample count mismatch")
    for family in ("wall_seconds", "internal_seconds"):
        metrics = summary.get(family)
        require(isinstance(metrics, dict), f"{label}.{family} missing")
        for name in ("p50", "p95", "p99", "maximum"):
            finite_non_negative(metrics.get(name), f"{label}.{family}.{name}")
    rss = summary.get("peak_rss_bytes")
    require(isinstance(rss, dict), f"{label}.peak_rss_bytes missing")
    for name in ("p50", "maximum"):
        finite_non_negative(rss.get(name), f"{label}.peak_rss_bytes.{name}")
    pss = summary.get("peak_pss_bytes")
    if platform_name == "linux":
        require(isinstance(pss, dict), f"{label}.peak_pss_bytes missing on Linux")
        for name in ("p50", "maximum"):
            finite_non_negative(pss.get(name), f"{label}.peak_pss_bytes.{name}")
    else:
        require(pss is None or isinstance(pss, dict), f"{label}.peak_pss_bytes invalid")


def validate_report(report: dict[str, Any], platform_name: str) -> dict[str, int]:
    require(report.get("schema") == RUN_SCHEMA, "paired-run schema mismatch")
    require(report.get("platform") == platform_name, "platform mismatch")
    for key in (
        "exact_store_tree_parity",
        "exact_export_parity",
        "zero_shadow_mismatches",
        "all_fault_classes_detected",
    ):
        require(report.get(key) is True, f"{key} is false")
    payload_sha = str(report.get("payload_sha256", ""))
    require(len(payload_sha) == 64, "payload SHA-256 invalid")
    claimed_report_sha = str(report.get("report_sha256", ""))
    require(len(claimed_report_sha) == 64, "report SHA-256 invalid")
    unhashed = dict(report)
    unhashed.pop("report_sha256", None)
    require(
        hashlib.sha256(canonical_bytes(unhashed)).hexdigest() == claimed_report_sha,
        "report SHA-256 does not match canonical content",
    )

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

    operations = report.get("operations")
    require(isinstance(operations, dict) and tuple(operations) == OPERATIONS, "operation set/order mismatch")
    import_telemetry = operations["import"].get("shadow_telemetry")
    require(
        isinstance(import_telemetry, list) and len(import_telemetry) == samples,
        "import telemetry sample count mismatch",
    )
    chunks = int(import_telemetry[0].get("chunk_encode_checks", 0))
    require(chunks >= records, "chunk descriptor count is below record count")

    semantic_hashes: dict[str, str] = {}
    for operation in OPERATIONS:
        item = operations[operation]
        semantic_sha = str(item.get("semantic_sha256", ""))
        require(len(semantic_sha) == 64, f"{operation} semantic SHA invalid")
        semantic_hashes[operation] = semantic_sha
        telemetry_samples = item.get("shadow_telemetry")
        require(
            isinstance(telemetry_samples, list) and len(telemetry_samples) == samples,
            f"{operation} telemetry sample count mismatch",
        )
        expected = expected_telemetry(operation, records, chunks)
        for telemetry in telemetry_samples:
            require(isinstance(telemetry, dict), f"{operation} telemetry is not an object")
            require(telemetry.get("enabled") is True, f"{operation} shadow disabled")
            require(int(telemetry.get("mismatches", -1)) == 0, f"{operation} mismatch recorded")
            require(telemetry.get("first_mismatch") == "None", f"{operation} mismatch latched")
            for counter, expected_value in expected.items():
                require(
                    int(telemetry.get(counter, -1)) == expected_value,
                    f"{operation} {counter} mismatch",
                )
        validate_summary(item.get("baseline"), f"{operation}.baseline", samples, platform_name)
        validate_summary(item.get("shadow"), f"{operation}.shadow", samples, platform_name)

    canonical_store = report.get("canonical_store")
    require(isinstance(canonical_store, dict), "canonical store manifest missing")
    file_count = int(canonical_store.get("file_count", 0))
    tree_sha = str(canonical_store.get("tree_sha256", ""))
    require(file_count > 0, "canonical store is empty")
    require(len(tree_sha) == 64, "store tree SHA invalid")
    files = canonical_store.get("files")
    require(isinstance(files, list) and len(files) == file_count, "store file inventory mismatch")

    import_pairs = report.get("import_pairs")
    require(isinstance(import_pairs, list) and len(import_pairs) == samples, "import pair count mismatch")
    for index, item in enumerate(import_pairs):
        require(int(item.get("sample", -1)) == index, "import sample index mismatch")
        require(item.get("semantic_sha256") == semantic_hashes["import"], "import semantic hash mismatch")
        require(item.get("store_tree_sha256") == tree_sha, "store tree changed between samples")
        require(int(item.get("store_file_count", 0)) == file_count, "store file count changed")

    export_pairs = report.get("export_pairs")
    require(isinstance(export_pairs, list) and len(export_pairs) == samples, "export pair count mismatch")
    for index, item in enumerate(export_pairs):
        require(int(item.get("sample", -1)) == index, "export sample index mismatch")
        require(int(item.get("bytes", -1)) == logical_bytes, "exported payload size mismatch")
        require(item.get("sha256") == payload_sha, "exported payload SHA mismatch")

    faults = report.get("faults")
    require(isinstance(faults, dict) and set(faults) == set(FAULTS), "fault class set mismatch")
    fault_counter = {
        "record-encode": "record_encode_checks",
        "record-decode": "record_decode_checks",
        "chunk-encode": "chunk_encode_checks",
        "chunk-decode": "chunk_decode_checks",
    }
    for name, mismatch in FAULTS.items():
        item = faults[name]
        require(item.get("expected_first_mismatch") == mismatch, f"{name} expected class mismatch")
        telemetry = item.get("telemetry")
        require(isinstance(telemetry, dict), f"{name} telemetry missing")
        require(telemetry.get("enabled") is True, f"{name} shadow disabled")
        require(int(telemetry.get("mismatches", 0)) == 1, f"{name} mismatch count invalid")
        require(telemetry.get("first_mismatch") == mismatch, f"{name} latch mismatch")
        for counter in COUNTERS:
            expected_value = 1 if counter == fault_counter[name] else 0
            require(int(telemetry.get(counter, -1)) == expected_value, f"{name} {counter} mismatch")

    return {
        "logical_bytes": logical_bytes,
        "records": records,
        "chunks": chunks,
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
    results: list[dict[str, Any]] = []
    total_baseline = 0.0
    total_shadow = 0.0
    for operation in OPERATIONS:
        item = report["operations"][operation]
        baseline_wall = item["baseline"]["wall_seconds"]
        shadow_wall = item["shadow"]["wall_seconds"]
        performance = {
            name: metric_gate(
                shadow_wall[name],
                baseline_wall[name],
                label=f"{operation}.{name}",
                max_ratio={
                    "p50": max_p50_ratio,
                    "p95": max_p95_ratio,
                    "p99": max_p99_ratio,
                    "maximum": max_maximum_ratio,
                }[name],
                max_delta=max_wall_delta_seconds,
            )
            for name in ("p50", "p95", "p99", "maximum")
        }
        total_baseline += float(baseline_wall["p50"])
        total_shadow += float(shadow_wall["p50"])
        memory_name = "peak_pss_bytes" if platform_name == "linux" else "peak_rss_bytes"
        baseline_memory = item["baseline"].get(memory_name)
        shadow_memory = item["shadow"].get(memory_name)
        require(isinstance(baseline_memory, dict), f"{operation} baseline {memory_name} missing")
        require(isinstance(shadow_memory, dict), f"{operation} shadow {memory_name} missing")
        memory = {
            "metric": memory_name,
            name: metric_gate(
                shadow_memory[name],
                baseline_memory[name],
                label=f"{operation}.{memory_name}.{name}",
                max_ratio=max_memory_ratio,
                max_delta=float(max_memory_delta_bytes),
            )
            for name in ("p50", "maximum")
        }
        results.append(
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
            "policy": "ratio-or-absolute-delta",
        },
        "operations": results,
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
    print(json.dumps({
        "schema": manifest["schema"],
        "platform": manifest["platform"],
        "slice_ready": manifest["slice_ready"],
        "manifest_sha256": manifest["manifest_sha256"],
    }, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
