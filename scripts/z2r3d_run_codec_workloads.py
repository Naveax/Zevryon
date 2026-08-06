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


def object_sha256(value: Any) -> str:
    return hashlib.sha256(canonical_bytes(value)).hexdigest()


def file_sha256(path: Path, chunk_bytes: int = 4 * 1024 * 1024) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(chunk_bytes):
            digest.update(chunk)
    return digest.hexdigest()


def percentile(values: list[float], fraction: float) -> float:
    if not values:
        raise WorkloadError("empty sample set")
    ordered = sorted(values)
    position = (len(ordered) - 1) * fraction
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    weight = position - lower
    return ordered[lower] * (1.0 - weight) + ordered[upper] * weight


def summarize(samples: list[dict[str, Any]]) -> dict[str, Any]:
    wall = [float(item["wall_seconds"]) for item in samples]
    internal = [float(item["result"]["seconds"]) for item in samples]
    rss = [int(item["peak_rss_bytes"]) for item in samples]
    pss = [
        int(item["peak_pss_bytes"])
        for item in samples
        if item.get("peak_pss_bytes") is not None
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
            {"p50": int(statistics.median(pss)), "maximum": max(pss)}
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
    peak_rss = 0
    peak_pss: int | None = None
    stop = threading.Event()

    def sample_memory(observed: psutil.Process) -> None:
        nonlocal peak_rss, peak_pss
        peak_rss = max(peak_rss, int(observed.memory_info().rss))
        try:
            pss = getattr(observed.memory_full_info(), "pss", None)
            if pss is not None:
                peak_pss = max(peak_pss or 0, int(pss))
        except (psutil.AccessDenied, psutil.NoSuchProcess, AttributeError):
            pass

    def monitor() -> None:
        try:
            observed = psutil.Process(process.pid)
        except psutil.Error:
            return
        while not stop.is_set():
            try:
                sample_memory(observed)
            except (psutil.AccessDenied, psutil.NoSuchProcess):
                break
            if process.poll() is not None:
                break
            time.sleep(0.005)

    thread = threading.Thread(target=monitor, daemon=True)
    thread.start()
    try:
        sample_memory(psutil.Process(process.pid))
    except (psutil.AccessDenied, psutil.NoSuchProcess):
        pass
    stdout, stderr = process.communicate()
    stop.set()
    thread.join(timeout=1.0)
    if process.returncode != 0:
        raise WorkloadError(
            f"command failed ({process.returncode}): {' '.join(command)}\n"
            f"stdout:\n{stdout.strip() or '<empty>'}\n"
            f"stderr:\n{stderr.strip() or '<empty>'}"
        )
    try:
        result = json.loads(stdout)
    except json.JSONDecodeError as error:
        raise WorkloadError(f"invalid workload JSON: {' '.join(command)}") from error
    if not isinstance(result, dict):
        raise WorkloadError("workload result is not an object")
    return {
        "wall_seconds": time.perf_counter() - started,
        "peak_rss_bytes": peak_rss,
        "peak_pss_bytes": peak_pss,
        "result": result,
        "stderr": stderr.strip(),
    }


def tree_manifest(root: Path) -> dict[str, Any]:
    files = [
        {
            "path": path.relative_to(root).as_posix(),
            "bytes": path.stat().st_size,
            "sha256": file_sha256(path),
        }
        for path in sorted(item for item in root.rglob("*") if item.is_file())
    ]
    manifest: dict[str, Any] = {"file_count": len(files), "files": files}
    manifest["tree_sha256"] = object_sha256(manifest)
    return manifest


def semantic_result(result: dict[str, Any]) -> dict[str, Any]:
    normalized = dict(result)
    normalized.pop("mode", None)
    normalized.pop("seconds", None)
    normalized.pop("shadow", None)
    return normalized


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


def validate_envelope(sample: dict[str, Any], mode: str, operation: str) -> dict[str, Any]:
    result = sample["result"]
    if result.get("schema") != WORKLOAD_SCHEMA:
        raise WorkloadError(f"{mode} {operation} schema mismatch")
    if result.get("mode") != mode or result.get("operation") != operation:
        raise WorkloadError(f"{mode} {operation} identity mismatch")
    store = result.get("store")
    if not isinstance(store, dict):
        raise WorkloadError(f"{mode} {operation} store stats missing")
    shadow = result.get("shadow")
    if not isinstance(shadow, dict):
        raise WorkloadError(f"{mode} {operation} telemetry missing")
    if shadow.get("enabled") is not (mode == "shadow"):
        raise WorkloadError(f"{mode} {operation} enablement mismatch")
    if int(shadow.get("mismatches", -1)) != 0 or shadow.get("first_mismatch") != "None":
        raise WorkloadError(f"{mode} {operation} recorded a mismatch")
    return result


def require_same_semantics(
    baseline: dict[str, Any], shadow: dict[str, Any], operation: str
) -> str:
    left = semantic_result(baseline["result"])
    right = semantic_result(shadow["result"])
    if left != right:
        raise WorkloadError(f"{operation} semantic output diverged")
    return object_sha256(left)


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
    raise WorkloadError(f"unknown operation {operation}")


def validate_telemetry(result: dict[str, Any], operation: str) -> None:
    store = result["store"]
    actual = result["shadow"]
    expected = expected_telemetry(
        operation,
        int(store["logical_records"]),
        int(store["chunk_count"]),
    )
    for key, value in expected.items():
        if int(actual.get(key, -1)) != value:
            raise WorkloadError(
                f"{operation} telemetry mismatch for {key}: "
                f"expected {value}, got {actual.get(key)}"
            )


def validate_fault(result: dict[str, Any], fault: str) -> None:
    if result.get("schema") != WORKLOAD_SCHEMA or result.get("operation") != "fault":
        raise WorkloadError(f"{fault} envelope mismatch")
    if result.get("mode") != "shadow" or result.get("fault") != fault:
        raise WorkloadError(f"{fault} identity mismatch")
    shadow = result.get("shadow")
    if not isinstance(shadow, dict) or shadow.get("enabled") is not True:
        raise WorkloadError(f"{fault} telemetry missing")
    if int(shadow.get("mismatches", 0)) != 1:
        raise WorkloadError(f"{fault} did not record one mismatch")
    if shadow.get("first_mismatch") != FAULTS[fault]:
        raise WorkloadError(f"{fault} latched {shadow.get('first_mismatch')}")
    expected_counter = {
        "record-encode": "record_encode_checks",
        "record-decode": "record_decode_checks",
        "chunk-encode": "chunk_encode_checks",
        "chunk-decode": "chunk_decode_checks",
    }[fault]
    counters = (
        "record_encode_checks",
        "record_decode_checks",
        "chunk_encode_checks",
        "chunk_decode_checks",
    )
    for counter in counters:
        expected = 1 if counter == expected_counter else 0
        if int(shadow.get(counter, -1)) != expected:
            raise WorkloadError(f"{fault} counter mismatch for {counter}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Run paired Z2R-3D codec workloads")
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
        raise WorkloadError("logical corpus must be at least 128 MiB")
    if args.giant_record_bytes < 64 * 1024 * 1024:
        raise WorkloadError("a real 64 MiB record is required")
    if args.records < 100_000:
        raise WorkloadError("at least 100,000 records are required")

    if args.work_dir.exists():
        shutil.rmtree(args.work_dir)
    args.work_dir.mkdir(parents=True)
    baseline_binary = str(args.baseline_binary.resolve())
    shadow_binary = str(args.shadow_binary.resolve())
    samples: dict[str, dict[str, list[dict[str, Any]]]] = {
        name: {"baseline": [], "shadow": []} for name in OPERATIONS
    }
    semantic_hashes: dict[str, str] = {}
    import_pairs: list[dict[str, Any]] = []
    baseline_store: Path | None = None
    shadow_store: Path | None = None

    for index in range(args.samples):
        left_store = args.work_dir / f"baseline-import-{index}"
        right_store = args.work_dir / f"shadow-import-{index}"
        dimensions = [
            str(args.logical_bytes),
            str(args.records),
            str(args.segment_bytes),
            str(args.giant_record_bytes),
        ]
        left, right = run_pair(
            [baseline_binary, "baseline", "import", str(left_store), *dimensions],
            [shadow_binary, "shadow", "import", str(right_store), *dimensions],
            shadow_first=index % 2 == 1,
        )
        validate_envelope(left, "baseline", "import")
        right_result = validate_envelope(right, "shadow", "import")
        validate_telemetry(right_result, "import")
        semantic = require_same_semantics(left, right, "import")
        semantic_hashes.setdefault("import", semantic)
        if semantic_hashes["import"] != semantic:
            raise WorkloadError("import semantics changed between samples")
        left_tree = tree_manifest(left_store)
        right_tree = tree_manifest(right_store)
        if left_tree != right_tree:
            raise WorkloadError("baseline and shadow store trees differ")
        import_pairs.append(
            {
                "sample": index,
                "semantic_sha256": semantic,
                "store_tree_sha256": left_tree["tree_sha256"],
                "store_file_count": left_tree["file_count"],
            }
        )
        samples["import"]["baseline"].append(left)
        samples["import"]["shadow"].append(right)
        if index == 0:
            baseline_store = left_store
            shadow_store = right_store
        else:
            shutil.rmtree(left_store)
            shutil.rmtree(right_store)

    assert baseline_store is not None and shadow_store is not None

    for operation in ("open", "verify"):
        for index in range(args.samples):
            left, right = run_pair(
                [baseline_binary, "baseline", operation, str(baseline_store)],
                [shadow_binary, "shadow", operation, str(shadow_store)],
                shadow_first=index % 2 == 1,
            )
            validate_envelope(left, "baseline", operation)
            right_result = validate_envelope(right, "shadow", operation)
            validate_telemetry(right_result, operation)
            semantic = require_same_semantics(left, right, operation)
            semantic_hashes.setdefault(operation, semantic)
            if semantic_hashes[operation] != semantic:
                raise WorkloadError(f"{operation} semantics changed between samples")
            samples[operation]["baseline"].append(left)
            samples[operation]["shadow"].append(right)

    export_pairs: list[dict[str, Any]] = []
    for index in range(args.samples):
        left_output = args.work_dir / f"baseline-export-{index}.bin"
        right_output = args.work_dir / f"shadow-export-{index}.bin"
        left, right = run_pair(
            [baseline_binary, "baseline", "export", str(baseline_store), str(left_output)],
            [shadow_binary, "shadow", "export", str(shadow_store), str(right_output)],
            shadow_first=index % 2 == 1,
        )
        validate_envelope(left, "baseline", "export")
        right_result = validate_envelope(right, "shadow", "export")
        validate_telemetry(right_result, "export")
        semantic = require_same_semantics(left, right, "export")
        semantic_hashes.setdefault("export", semantic)
        if semantic_hashes["export"] != semantic:
            raise WorkloadError("export semantics changed between samples")
        left_sha = file_sha256(left_output)
        right_sha = file_sha256(right_output)
        if left_sha != right_sha or left_output.stat().st_size != right_output.stat().st_size:
            raise WorkloadError("exported payloads differ")
        if left_sha != left["result"]["store"]["payload_sha256"]:
            raise WorkloadError("export SHA-256 differs from manifest payload SHA-256")
        export_pairs.append(
            {"sample": index, "bytes": left_output.stat().st_size, "sha256": left_sha}
        )
        samples["export"]["baseline"].append(left)
        samples["export"]["shadow"].append(right)
        left_output.unlink()
        right_output.unlink()

    faults: dict[str, Any] = {}
    for fault, mismatch in FAULTS.items():
        measured = run_measured([shadow_binary, "shadow", "fault", fault])
        validate_fault(measured["result"], fault)
        faults[fault] = {
            "expected_first_mismatch": mismatch,
            "telemetry": measured["result"]["shadow"],
        }

    operations = {
        operation: {
            "semantic_sha256": semantic_hashes[operation],
            "baseline": summarize(samples[operation]["baseline"]),
            "shadow": summarize(samples[operation]["shadow"]),
            "shadow_telemetry": [
                item["result"]["shadow"] for item in samples[operation]["shadow"]
            ],
        }
        for operation in OPERATIONS
    }
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
        "faults": faults,
        "canonical_store": tree_manifest(baseline_store),
        "payload_sha256": samples["import"]["baseline"][0]["result"]["store"][
            "payload_sha256"
        ],
        "exact_store_tree_parity": True,
        "exact_export_parity": True,
        "zero_shadow_mismatches": True,
        "all_fault_classes_detected": True,
    }
    report["report_sha256"] = object_sha256(report)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
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
