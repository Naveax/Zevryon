#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import shutil
import statistics
import subprocess
import threading
import time
from typing import Any

import psutil

RUN_SCHEMA = "zevryon.z2r3d.paired-run.v1"
WORKLOAD_SCHEMA = "zevryon.z2r3d.codec-workload.v1"
OPERATIONS = ("import", "open", "verify", "export")
FAULTS = {
    "record-encode": "RecordEncode",
    "record-decode": "RecordDecode",
    "chunk-encode": "ChunkEncode",
    "chunk-decode": "ChunkDecode",
}


class WorkloadError(RuntimeError):
    pass


def canonical_bytes(value: Any) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode(
        "utf-8"
    )


def sha256_bytes(value: Any) -> str:
    return hashlib.sha256(canonical_bytes(value)).hexdigest()


def sha256_file(path: Path, chunk_bytes: int = 4 * 1024 * 1024) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while True:
            chunk = stream.read(chunk_bytes)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def percentile(values: list[float], fraction: float) -> float:
    if not values:
        raise WorkloadError("cannot calculate a percentile from an empty sample set")
    ordered = sorted(values)
    position = (len(ordered) - 1) * fraction
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    weight = position - lower
    return ordered[lower] * (1.0 - weight) + ordered[upper] * weight


def summarize(samples: list[dict[str, Any]]) -> dict[str, Any]:
    wall = [float(sample["wall_seconds"]) for sample in samples]
    internal = [float(sample["result"]["seconds"]) for sample in samples]
    rss = [int(sample["peak_rss_bytes"]) for sample in samples]
    pss = [
        int(sample["peak_pss_bytes"])
        for sample in samples
        if sample.get("peak_pss_bytes") is not None
    ]
    return {
        "sample_count": len(samples),
        "wall_seconds": {
            "p50": statistics.median(wall),
            "p95": percentile(wall, 0.95),
            "p99": percentile(wall, 0.99),
            "maximum": max(wall),
        },
        "internal_seconds": {
            "p50": statistics.median(internal),
            "p95": percentile(internal, 0.95),
            "p99": percentile(internal, 0.99),
            "maximum": max(internal),
        },
        "peak_rss_bytes": {
            "p50": int(statistics.median(rss)),
            "maximum": max(rss),
        },
        "peak_pss_bytes": (
            {
                "p50": int(statistics.median(pss)),
                "maximum": max(pss),
            }
            if len(pss) == len(samples)
            else None
        ),
    }


def run_measured(command: list[str]) -> dict[str, Any]:
    started = time.perf_counter()
    process = subprocess.Popen(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    peak_rss_bytes = 0
    peak_pss_bytes: int | None = None
    stop = threading.Event()

    def monitor() -> None:
        nonlocal peak_rss_bytes, peak_pss_bytes
        try:
            observed = psutil.Process(process.pid)
        except psutil.Error:
            return
        while not stop.is_set():
            try:
                memory = observed.memory_info()
                peak_rss_bytes = max(peak_rss_bytes, int(memory.rss))
                try:
                    full = observed.memory_full_info()
                    pss = getattr(full, "pss", None)
                    if pss is not None:
                        peak_pss_bytes = max(peak_pss_bytes or 0, int(pss))
                except (psutil.AccessDenied, psutil.NoSuchProcess, AttributeError):
                    pass
            except (psutil.AccessDenied, psutil.NoSuchProcess):
                break
            if process.poll() is not None:
                break
            time.sleep(0.005)

    thread = threading.Thread(target=monitor, daemon=True)
    thread.start()
    stdout, stderr = process.communicate()
    stop.set()
    thread.join(timeout=1.0)
    wall_seconds = time.perf_counter() - started
    if process.returncode != 0:
        raise WorkloadError(
            f"command failed ({process.returncode}): {' '.join(command)}\n"
            f"--- stdout ---\n{stdout.strip() or '<empty>'}\n"
            f"--- stderr ---\n{stderr.strip() or '<empty>'}"
        )
    try:
        result = json.loads(stdout)
    except json.JSONDecodeError as error:
        raise WorkloadError(f"command returned invalid JSON: {' '.join(command)}") from error
    if not isinstance(result, dict):
        raise WorkloadError("workload result is not a JSON object")
    return {
        "command": command,
        "wall_seconds": wall_seconds,
        "peak_rss_bytes": peak_rss_bytes,
        "peak_pss_bytes": peak_pss_bytes,
        "result": result,
        "stderr": stderr.strip(),
    }


def validate_result(
    sample: dict[str, Any],
    *,
    mode: str,
    operation: str,
) -> dict[str, Any]:
    result = sample["result"]
    if result.get("schema") != WORKLOAD_SCHEMA:
        raise WorkloadError(f"{mode} {operation} workload schema mismatch")
    if result.get("mode") != mode or result.get("operation") != operation:
        raise WorkloadError(f"{mode} {operation} identity mismatch")
    shadow = result.get("shadow")
    if not isinstance(shadow, dict):
        raise WorkloadError(f"{mode} {operation} shadow telemetry missing")
    expected_enabled = mode == "shadow"
    if shadow.get("enabled") is not expected_enabled:
        raise WorkloadError(f"{mode} {operation} shadow enablement mismatch")
    if int(shadow.get("mismatches", -1)) != 0:
        raise WorkloadError(f"{mode} {operation} recorded a codec mismatch")
    if shadow.get("first_mismatch") != "None":
        raise WorkloadError(f"{mode} {operation} latched an unexpected mismatch")
    store = result.get("store")
    if operation != "fault" and not isinstance(store, dict):
        raise WorkloadError(f"{mode} {operation} store stats missing")
    return result


def semantic_result(result: dict[str, Any]) -> dict[str, Any]:
    normalized = dict(result)
    normalized.pop("mode", None)
    normalized.pop("seconds", None)
    normalized.pop("shadow", None)
    return normalized


def tree_manifest(root: Path) -> dict[str, Any]:
    files: list[dict[str, Any]] = []
    for path in sorted(item for item in root.rglob("*") if item.is_file()):
        files.append(
            {
                "path": path.relative_to(root).as_posix(),
                "bytes": path.stat().st_size,
                "sha256": sha256_file(path),
            }
        )
    manifest = {"files": files, "file_count": len(files)}
    manifest["tree_sha256"] = sha256_bytes(manifest)
    return manifest


def run_pair(
    baseline_command: list[str],
    shadow_command: list[str],
    *,
    shadow_first: bool,
) -> tuple[dict[str, Any], dict[str, Any]]:
    if shadow_first:
        shadow = run_measured(shadow_command)
        baseline = run_measured(baseline_command)
    else:
        baseline = run_measured(baseline_command)
        shadow = run_measured(shadow_command)
    return baseline, shadow


def require_same_semantics(
    baseline: dict[str, Any],
    shadow: dict[str, Any],
    operation: str,
) -> str:
    baseline_semantic = semantic_result(baseline["result"])
    shadow_semantic = semantic_result(shadow["result"])
    if baseline_semantic != shadow_semantic:
        raise WorkloadError(f"{operation} semantic output diverged")
    return sha256_bytes(baseline_semantic)


def validate_import_telemetry(result: dict[str, Any]) -> None:
    store = result["store"]
    shadow = result["shadow"]
    if int(shadow["record_encode_checks"]) != int(store["logical_records"]):
        raise WorkloadError("import record encode count diverged")
    if int(shadow["chunk_encode_checks"]) != int(store["chunk_count"]):
        raise WorkloadError("import chunk encode count diverged")


def validate_reader_telemetry(result: dict[str, Any], operation: str) -> None:
    shadow = result["shadow"]
    if int(shadow["record_decode_checks"]) <= 0:
        raise WorkloadError(f"{operation} observed no record decode operations")
    if int(shadow["chunk_decode_checks"]) <= 0:
        raise WorkloadError(f"{operation} observed no chunk decode operations")


def validate_fault(result: dict[str, Any], fault: str) -> None:
    if result.get("schema") != WORKLOAD_SCHEMA or result.get("operation") != "fault":
        raise WorkloadError(f"{fault} returned an invalid workload envelope")
    if result.get("mode") != "shadow" or result.get("fault") != fault:
        raise WorkloadError(f"{fault} identity mismatch")
    shadow = result.get("shadow")
    if not isinstance(shadow, dict) or shadow.get("enabled") is not True:
        raise WorkloadError(f"{fault} shadow telemetry missing")
    if int(shadow.get("mismatches", 0)) != 1:
        raise WorkloadError(f"{fault} did not record exactly one mismatch")
    if shadow.get("first_mismatch") != FAULTS[fault]:
        raise WorkloadError(f"{fault} latched the wrong mismatch class")


def main() -> int:
    parser = argparse.ArgumentParser(description="Run paired Z2R-3D MassiveDoc codec workloads")
    parser.add_argument("--baseline-binary", type=Path, required=True)
    parser.add_argument("--shadow-binary", type=Path, required=True)
    parser.add_argument("--platform", choices=("linux", "windows", "macos"), required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--logical-bytes", type=int, default=128 * 1024 * 1024)
    parser.add_argument("--records", type=int, default=131_072)
    parser.add_argument("--segment-bytes", type=int, default=16 * 1024 * 1024)
    parser.add_argument("--giant-record-bytes", type=int, default=64 * 1024 * 1024)
    parser.add_argument("--samples", type=int, default=3)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    if args.samples < 3:
        raise WorkloadError("at least three paired samples are required")
    if args.logical_bytes < 128 * 1024 * 1024:
        raise WorkloadError("promotion workload must contain at least 128 MiB")
    if args.giant_record_bytes < 64 * 1024 * 1024:
        raise WorkloadError("promotion workload must contain a real 64 MiB record")
    if args.records < 100_000:
        raise WorkloadError("promotion workload must contain at least 100,000 records")

    if args.work_dir.exists():
        shutil.rmtree(args.work_dir)
    args.work_dir.mkdir(parents=True)
    baseline_binary = str(args.baseline_binary.resolve())
    shadow_binary = str(args.shadow_binary.resolve())

    operation_samples: dict[str, dict[str, list[dict[str, Any]]]] = {
        operation: {"baseline": [], "shadow": []} for operation in OPERATIONS
    }
    semantic_hashes: dict[str, str] = {}
    import_pairs: list[dict[str, Any]] = []
    canonical_baseline_store: Path | None = None
    canonical_shadow_store: Path | None = None

    for index in range(args.samples):
        baseline_store = args.work_dir / f"baseline-import-{index}"
        shadow_store = args.work_dir / f"shadow-import-{index}"
        common = [
            str(args.logical_bytes),
            str(args.records),
            str(args.segment_bytes),
            str(args.giant_record_bytes),
        ]
        baseline, shadow = run_pair(
            [baseline_binary, "baseline", "import", str(baseline_store), *common],
            [shadow_binary, "shadow", "import", str(shadow_store), *common],
            shadow_first=index % 2 == 1,
        )
        validate_result(baseline, mode="baseline", operation="import")
        shadow_result = validate_result(shadow, mode="shadow", operation="import")
        validate_import_telemetry(shadow_result)
        semantic_hash = require_same_semantics(baseline, shadow, "import")
        semantic_hashes.setdefault("import", semantic_hash)
        if semantic_hashes["import"] != semantic_hash:
            raise WorkloadError("import semantic output changed between samples")
        baseline_tree = tree_manifest(baseline_store)
        shadow_tree = tree_manifest(shadow_store)
        if baseline_tree != shadow_tree:
            raise WorkloadError("baseline and shadow store trees differ")
        import_pairs.append(
            {
                "sample": index,
                "semantic_sha256": semantic_hash,
                "store_tree_sha256": baseline_tree["tree_sha256"],
                "store_file_count": baseline_tree["file_count"],
            }
        )
        operation_samples["import"]["baseline"].append(baseline)
        operation_samples["import"]["shadow"].append(shadow)
        if index == 0:
            canonical_baseline_store = baseline_store
            canonical_shadow_store = shadow_store
        else:
            shutil.rmtree(baseline_store)
            shutil.rmtree(shadow_store)

    assert canonical_baseline_store is not None
    assert canonical_shadow_store is not None

    for operation in ("open", "verify"):
        for index in range(args.samples):
            baseline, shadow = run_pair(
                [baseline_binary, "baseline", operation, str(canonical_baseline_store)],
                [shadow_binary, "shadow", operation, str(canonical_shadow_store)],
                shadow_first=index % 2 == 1,
            )
            validate_result(baseline, mode="baseline", operation=operation)
            shadow_result = validate_result(shadow, mode="shadow", operation=operation)
            validate_reader_telemetry(shadow_result, operation)
            semantic_hash = require_same_semantics(baseline, shadow, operation)
            semantic_hashes.setdefault(operation, semantic_hash)
            if semantic_hashes[operation] != semantic_hash:
                raise WorkloadError(f"{operation} semantic output changed between samples")
            operation_samples[operation]["baseline"].append(baseline)
            operation_samples[operation]["shadow"].append(shadow)

    export_pairs: list[dict[str, Any]] = []
    for index in range(args.samples):
        baseline_export = args.work_dir / f"baseline-export-{index}.bin"
        shadow_export = args.work_dir / f"shadow-export-{index}.bin"
        baseline, shadow = run_pair(
            [
                baseline_binary,
                "baseline",
                "export",
                str(canonical_baseline_store),
                str(baseline_export),
            ],
            [
                shadow_binary,
                "shadow",
                "export",
                str(canonical_shadow_store),
                str(shadow_export),
            ],
            shadow_first=index % 2 == 1,
        )
        validate_result(baseline, mode="baseline", operation="export")
        shadow_result = validate_result(shadow, mode="shadow", operation="export")
        validate_reader_telemetry(shadow_result, "export")
        semantic_hash = require_same_semantics(baseline, shadow, "export")
        semantic_hashes.setdefault("export", semantic_hash)
        if semantic_hashes["export"] != semantic_hash:
            raise WorkloadError("export semantic output changed between samples")
        baseline_sha = sha256_file(baseline_export)
        shadow_sha = sha256_file(shadow_export)
        if baseline_sha != shadow_sha or baseline_export.stat().st_size != shadow_export.stat().st_size:
            raise WorkloadError("baseline and shadow exported payloads differ")
        if baseline_sha != baseline["result"]["store"]["payload_sha256"]:
            raise WorkloadError("exported payload differs from store payload SHA-256")
        export_pairs.append(
            {
                "sample": index,
                "bytes": baseline_export.stat().st_size,
                "sha256": baseline_sha,
            }
        )
        operation_samples["export"]["baseline"].append(baseline)
        operation_samples["export"]["shadow"].append(shadow)
        baseline_export.unlink()
        shadow_export.unlink()

    fault_results: dict[str, dict[str, Any]] = {}
    for fault, expected in FAULTS.items():
        measured = run_measured([shadow_binary, "shadow", "fault", fault])
        validate_fault(measured["result"], fault)
        fault_results[fault] = {
            "expected_first_mismatch": expected,
            "shadow": measured["result"]["shadow"],
        }

    operations: dict[str, Any] = {}
    for operation in OPERATIONS:
        baseline_samples = operation_samples[operation]["baseline"]
        shadow_samples = operation_samples[operation]["shadow"]
        operations[operation] = {
            "semantic_sha256": semantic_hashes[operation],
            "baseline": summarize(baseline_samples),
            "shadow": summarize(shadow_samples),
            "baseline_samples": baseline_samples,
            "shadow_samples": shadow_samples,
        }

    canonical_tree = tree_manifest(canonical_baseline_store)
    report: dict[str, Any] = {
        "schema": RUN_SCHEMA,
        "platform": args.platform,
        "parameters": {
            "logical_bytes": args.logical_bytes,
            "records": args.records,
            "segment_bytes": args.segment_bytes,
            "giant_record_bytes": args.giant_record_bytes,
            "samples": args.samples,
        },
        "operations": operations,
        "import_pairs": import_pairs,
        "export_pairs": export_pairs,
        "faults": fault_results,
        "canonical_store": canonical_tree,
        "payload_sha256": operation_samples["import"]["baseline"][0]["result"]["store"][
            "payload_sha256"
        ],
        "exact_store_tree_parity": True,
        "exact_export_parity": True,
        "zero_shadow_mismatches": True,
        "all_fault_classes_detected": True,
    }
    report["report_sha256"] = sha256_bytes(report)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(
        json.dumps(
            {
                "schema": report["schema"],
                "platform": report["platform"],
                "payload_sha256": report["payload_sha256"],
                "report_sha256": report["report_sha256"],
            },
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
