#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import threading
import time


def read_process_sample(pid: int) -> dict[str, int | float | None]:
    sample: dict[str, int | float | None] = {
        "pss_bytes": None,
        "read_bytes": 0,
        "write_bytes": 0,
        "minor_faults": 0,
        "major_faults": 0,
        "cpu_seconds": 0.0,
    }
    try:
        for line in Path(f"/proc/{pid}/smaps_rollup").read_text(
            encoding="ascii", errors="ignore"
        ).splitlines():
            if line.startswith("Pss:"):
                sample["pss_bytes"] = int(line.split()[1]) * 1024
                break
    except (FileNotFoundError, PermissionError, ProcessLookupError):
        pass
    try:
        for line in Path(f"/proc/{pid}/io").read_text(
            encoding="ascii", errors="ignore"
        ).splitlines():
            key, _, value = line.partition(":")
            if key == "read_bytes":
                sample["read_bytes"] = int(value.strip())
            elif key == "write_bytes":
                sample["write_bytes"] = int(value.strip())
    except (FileNotFoundError, PermissionError, ProcessLookupError):
        pass
    try:
        stat = Path(f"/proc/{pid}/stat").read_text(encoding="ascii")
        fields = stat[stat.rfind(")") + 2 :].split()
        sample["minor_faults"] = int(fields[7])
        sample["major_faults"] = int(fields[9])
        ticks = os.sysconf("SC_CLK_TCK")
        sample["cpu_seconds"] = (int(fields[11]) + int(fields[12])) / ticks
    except (FileNotFoundError, PermissionError, ProcessLookupError, ValueError, IndexError):
        pass
    return sample


def run_measured(command: list[str]) -> dict:
    started = time.perf_counter()
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    peak_pss_bytes = 0
    final_sample: dict[str, int | float | None] = {}
    stop = threading.Event()

    def monitor() -> None:
        nonlocal peak_pss_bytes, final_sample
        while not stop.is_set():
            sample = read_process_sample(process.pid)
            pss = sample.get("pss_bytes")
            if isinstance(pss, int):
                peak_pss_bytes = max(peak_pss_bytes, pss)
            final_sample = sample
            if process.poll() is not None:
                break
            time.sleep(0.002)

    thread = threading.Thread(target=monitor, daemon=True)
    thread.start()
    stdout, stderr = process.communicate()
    stop.set()
    thread.join(timeout=1)
    elapsed = time.perf_counter() - started
    if process.returncode != 0:
        raise RuntimeError(f"command failed ({process.returncode}): {' '.join(command)}\n{stderr}")
    payload = json.loads(stdout)
    return {
        "command": command,
        "seconds": elapsed,
        "peak_pss_bytes": peak_pss_bytes or None,
        "peak_pss_mb": peak_pss_bytes / 1_000_000 if peak_pss_bytes else None,
        "read_bytes": final_sample.get("read_bytes"),
        "write_bytes": final_sample.get("write_bytes"),
        "minor_faults": final_sample.get("minor_faults"),
        "major_faults": final_sample.get("major_faults"),
        "cpu_seconds": final_sample.get("cpu_seconds"),
        "result": payload,
        "stderr": stderr.strip(),
    }


def sha256_file(path: Path, chunk_bytes: int = 4 * 1024 * 1024) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while True:
            chunk = stream.read(chunk_bytes)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def throughput_mb_s(logical_bytes: int, seconds: float) -> float:
    return logical_bytes / 1_000_000 / seconds if seconds else 0.0


def viewport_command(binary: Path, store: Path, y_px: int, height_px: int, overscan_px: int) -> list[str]:
    return [str(binary), "viewport", str(store), str(y_px), str(height_px), str(overscan_px), "512"]


def main() -> int:
    parser = argparse.ArgumentParser(description="Run a reproducible MassiveDoc native benchmark")
    parser.add_argument("--binary", type=Path, default=Path("build/zevryon-massivedoc"))
    parser.add_argument("--work-dir", type=Path, default=Path(".massivedoc-benchmark"))
    parser.add_argument("--logical-bytes", type=int, default=64 * 1024 * 1024)
    parser.add_argument("--records", type=int, default=131_072)
    parser.add_argument("--segment-mib", type=int, default=16)
    parser.add_argument("--largest-record-limit-bytes", type=int, default=64 * 1024 * 1024)
    parser.add_argument("--giant-record-bytes", type=int, default=0)
    parser.add_argument("--viewport-height", type=int, default=720)
    parser.add_argument("--overscan", type=int, default=720)
    parser.add_argument("--cleanup-large-files", action="store_true")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    if args.work_dir.exists():
        shutil.rmtree(args.work_dir)
    args.work_dir.mkdir(parents=True)
    corpus = args.work_dir / "corpus.zmdoc"
    corpus_sidecar = corpus.with_suffix(corpus.suffix + ".json")
    store = args.work_dir / "store"
    exported = args.work_dir / "exported-payload.bin"
    generator = Path(__file__).with_name("generate_massivedoc_corpus.py")
    generator_command = [
        sys.executable,
        str(generator),
        str(corpus),
        "--logical-bytes",
        str(args.logical_bytes),
        "--records",
        str(args.records),
        "--largest-record-limit-bytes",
        str(args.largest_record_limit_bytes),
    ]
    if args.giant_record_bytes:
        generator_command.extend(["--giant-record-bytes", str(args.giant_record_bytes)])
    generated = subprocess.run(generator_command, check=True, capture_output=True, text=True)
    corpus_summary = json.loads(generated.stdout)

    imported = run_measured([str(args.binary), "import", str(corpus), str(store), str(args.segment_mib)])
    if args.cleanup_large_files:
        corpus.unlink(missing_ok=True)
        corpus_sidecar.unlink(missing_ok=True)
    searched = run_measured([str(args.binary), "search", str(store), corpus_summary["tail_marker"], "2"])
    verified = run_measured([str(args.binary), "verify", str(store)])
    exported_run = run_measured([str(args.binary), "export", str(store), str(exported)])
    arena = run_measured([str(args.binary), "arena-build", str(store), "96", "18"])

    total_height_q8 = arena["result"]["arena"]["total_height_q8"]
    total_height_px = total_height_q8 // 256
    positions = {
        "top": 0,
        "middle": max(0, total_height_px // 2),
        "end": max(0, total_height_px - args.viewport_height),
    }
    viewports = {
        name: run_measured(viewport_command(args.binary, store, position, args.viewport_height, args.overscan))
        for name, position in positions.items()
    }

    layout_window = None
    if args.giant_record_bytes:
        layout_script = Path(__file__).with_name("layout_window_benchmark.py")
        layout_output = args.work_dir / "layout-window-report.json"
        layout_process = subprocess.run(
            [
                sys.executable,
                str(layout_script),
                "--binary",
                str(args.binary),
                "--store",
                str(store),
                "--viewport-width",
                "800",
                "--viewport-height",
                str(args.viewport_height),
                "--overscan",
                str(args.overscan),
                "--max-fragments",
                "512",
                "--cache-mb",
                "8",
                "--expected-record",
                str(corpus_summary["giant_record_index"]),
                "--minimum-source-bytes",
                str(args.giant_record_bytes),
                "--output",
                str(layout_output),
            ],
            check=True,
            capture_output=True,
            text=True,
        )
        layout_window = json.loads(layout_process.stdout)

    store_summary = imported["result"]["store"]
    store_sha = store_summary["payload_sha256"]
    if store_sha != corpus_summary["payload_sha256"]:
        raise RuntimeError("store payload hash differs from generated corpus")
    export_sha = sha256_file(exported)
    if export_sha != corpus_summary["payload_sha256"]:
        raise RuntimeError("exported payload hash differs from generated corpus")
    if args.cleanup_large_files:
        exported.unlink(missing_ok=True)
    hits = searched["result"]["hits"]
    if len(hits) != 1 or hits[0]["record_index"] != args.records - 1:
        raise RuntimeError("tail marker was not found in final record")
    top_records = viewports["top"]["result"]["viewport"]["records"]
    end_records = viewports["end"]["result"]["viewport"]["records"]
    if not top_records or top_records[0]["record_index"] != 0:
        raise RuntimeError("top viewport does not begin at the first record")
    if not end_records or end_records[-1]["record_index"] != args.records - 1:
        raise RuntimeError("end viewport does not reach the final record")
    if any(item["result"]["viewport"]["count"] > 512 for item in viewports.values()):
        raise RuntimeError("viewport exceeded bounded materialization count")

    physical_store_bytes = int(store_summary["physical_bytes"])
    metadata_overhead_bytes = physical_store_bytes - args.logical_bytes
    report = {
        "schema": "zevryon.massivedoc.benchmark.v4",
        "logical_bytes": args.logical_bytes,
        "logical_records": args.records,
        "logical_nodes": corpus_summary["logical_nodes"],
        "style_runs": corpus_summary["style_runs"],
        "resource_references": corpus_summary["resource_references"],
        "largest_record_limit_bytes": corpus_summary["largest_record_limit_bytes"],
        "largest_record_observed_bytes": corpus_summary["largest_record_observed_bytes"],
        "average_record_bytes": corpus_summary["average_record_bytes"],
        "giant_record_bytes": corpus_summary["giant_record_bytes"],
        "payload_sha256": store_sha,
        "export_sha256": export_sha,
        "physical_store_bytes": physical_store_bytes,
        "metadata_overhead_bytes": metadata_overhead_bytes,
        "metadata_overhead_percent": metadata_overhead_bytes / args.logical_bytes * 100,
        "import": imported,
        "search": searched,
        "verify": verified,
        "export": exported_run,
        "arena_build": arena,
        "viewports": viewports,
        "layout_window": layout_window,
        "throughput_mb_s_decimal": {
            "import": throughput_mb_s(args.logical_bytes, float(imported["seconds"])),
            "verify": throughput_mb_s(args.logical_bytes, float(verified["seconds"])),
            "export": throughput_mb_s(args.logical_bytes, float(exported_run["seconds"])),
        },
        "bounded_viewport_materialization": True,
        "bounded_layout_fragment_materialization": layout_window is not None,
        "zero_data_loss": True,
        "tail_marker_in_final_record": True,
    }
    text = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text, encoding="utf-8")
    print(text, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
