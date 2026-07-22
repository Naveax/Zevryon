#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys
import threading
import time


def read_pss_mb(pid: int) -> float | None:
    path = Path(f"/proc/{pid}/smaps_rollup")
    try:
        for line in path.read_text(encoding="ascii", errors="ignore").splitlines():
            if line.startswith("Pss:"):
                return int(line.split()[1]) * 1024 / 1_000_000
    except (FileNotFoundError, PermissionError, ProcessLookupError):
        return None
    return None


def run_measured(command: list[str]) -> dict:
    started = time.perf_counter()
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    peak_pss_mb = 0.0
    stop = threading.Event()

    def monitor() -> None:
        nonlocal peak_pss_mb
        while not stop.is_set():
            value = read_pss_mb(process.pid)
            if value is not None:
                peak_pss_mb = max(peak_pss_mb, value)
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
        "peak_pss_mb": peak_pss_mb or None,
        "result": payload,
        "stderr": stderr.strip(),
    }


def viewport_command(binary: Path, store: Path, y_px: int, height_px: int, overscan_px: int) -> list[str]:
    return [str(binary), "viewport", str(store), str(y_px), str(height_px), str(overscan_px), "512"]


def main() -> int:
    parser = argparse.ArgumentParser(description="Run a reproducible MassiveDoc native benchmark")
    parser.add_argument("--binary", type=Path, default=Path("build/zevryon-massivedoc"))
    parser.add_argument("--work-dir", type=Path, default=Path(".massivedoc-benchmark"))
    parser.add_argument("--logical-bytes", type=int, default=64 * 1024 * 1024)
    parser.add_argument("--records", type=int, default=131_072)
    parser.add_argument("--segment-mib", type=int, default=16)
    parser.add_argument("--viewport-height", type=int, default=720)
    parser.add_argument("--overscan", type=int, default=720)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    if args.work_dir.exists():
        shutil.rmtree(args.work_dir)
    args.work_dir.mkdir(parents=True)
    corpus = args.work_dir / "corpus.zmdoc"
    store = args.work_dir / "store"
    generator = Path(__file__).with_name("generate_massivedoc_corpus.py")
    generated = subprocess.run(
        [sys.executable, str(generator), str(corpus), "--logical-bytes", str(args.logical_bytes),
         "--records", str(args.records)],
        check=True, capture_output=True, text=True,
    )
    corpus_summary = json.loads(generated.stdout)
    imported = run_measured([str(args.binary), "import", str(corpus), str(store), str(args.segment_mib)])
    searched = run_measured([str(args.binary), "search", str(store), corpus_summary["tail_marker"], "2"])
    verified = run_measured([str(args.binary), "verify", str(store)])
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

    store_sha = imported["result"]["store"]["payload_sha256"]
    if store_sha != corpus_summary["payload_sha256"]:
        raise RuntimeError("store payload hash differs from generated corpus")
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

    report = {
        "schema": "zevryon.massivedoc.benchmark.v2",
        "logical_bytes": args.logical_bytes,
        "logical_records": args.records,
        "payload_sha256": store_sha,
        "import": imported,
        "search": searched,
        "verify": verified,
        "arena_build": arena,
        "viewports": viewports,
        "bounded_viewport_materialization": True,
        "zero_data_loss": True,
    }
    text = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text, encoding="utf-8")
    print(text, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
