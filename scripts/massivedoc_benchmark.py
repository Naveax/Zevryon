#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
from pathlib import Path
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


def main() -> int:
    parser = argparse.ArgumentParser(description="Run a reproducible MassiveDoc native smoke benchmark")
    parser.add_argument("--binary", type=Path, default=Path("build/zevryon-massivedoc"))
    parser.add_argument("--work-dir", type=Path, default=Path(".massivedoc-benchmark"))
    parser.add_argument("--logical-bytes", type=int, default=64 * 1024 * 1024)
    parser.add_argument("--records", type=int, default=131_072)
    parser.add_argument("--segment-mib", type=int, default=16)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    args.work_dir.mkdir(parents=True, exist_ok=True)
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
    store_sha = imported["result"]["store"]["payload_sha256"]
    if store_sha != corpus_summary["payload_sha256"]:
        raise RuntimeError("store payload hash differs from generated corpus")
    hits = searched["result"]["hits"]
    if len(hits) != 1 or hits[0]["record_index"] != args.records - 1:
        raise RuntimeError("tail marker was not found in final record")
    report = {
        "schema": "zevryon.massivedoc.benchmark.v1",
        "logical_bytes": args.logical_bytes,
        "logical_records": args.records,
        "payload_sha256": store_sha,
        "import": imported,
        "search": searched,
        "verify": verified,
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
