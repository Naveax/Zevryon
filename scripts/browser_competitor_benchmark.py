#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import multiprocessing as mp
import os
from pathlib import Path
import queue
import statistics
import threading
import time
import traceback
from typing import Any

import psutil
from playwright.sync_api import sync_playwright


def read_pss_bytes(pid: int) -> int:
    try:
        for line in Path(f"/proc/{pid}/smaps_rollup").read_text(
            encoding="ascii", errors="ignore"
        ).splitlines():
            if line.startswith("Pss:"):
                return int(line.split()[1]) * 1024
    except (FileNotFoundError, PermissionError, ProcessLookupError, ValueError):
        pass
    try:
        return int(psutil.Process(pid).memory_info().rss)
    except (psutil.Error, ProcessLookupError):
        return 0


def process_tree_pss_bytes(root_pid: int) -> int:
    try:
        root = psutil.Process(root_pid)
        processes = [root, *root.children(recursive=True)]
    except psutil.Error:
        return 0
    total = 0
    seen: set[int] = set()
    for process in processes:
        if process.pid in seen:
            continue
        seen.add(process.pid)
        total += read_pss_bytes(process.pid)
    return total


class PeakPssMonitor:
    def __init__(self, root_pid: int) -> None:
        self.root_pid = root_pid
        self.peak_bytes = 0
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)

    def _run(self) -> None:
        while not self._stop.is_set():
            self.peak_bytes = max(
                self.peak_bytes, process_tree_pss_bytes(self.root_pid)
            )
            time.sleep(0.01)

    def start(self) -> None:
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        self._thread.join(timeout=2.0)
        self.peak_bytes = max(
            self.peak_bytes, process_tree_pss_bytes(self.root_pid)
        )


def percentile(values: list[float], value: float) -> float:
    ordered = sorted(values)
    if not ordered:
        raise ValueError("percentile requires samples")
    position = (len(ordered) - 1) * value / 100.0
    lower = int(position)
    upper = min(lower + 1, len(ordered) - 1)
    fraction = position - lower
    return ordered[lower] * (1.0 - fraction) + ordered[upper] * fraction


def deterministic_offsets(payload_bytes: int, slice_bytes: int, count: int) -> list[int]:
    maximum = max(0, payload_bytes - slice_bytes)
    state = 0x243F6A88
    output: list[int] = []
    for _ in range(count):
        state = (state * 1664525 + 1013904223) & 0xFFFFFFFF
        output.append((state * maximum) // 0xFFFFFFFF if maximum else 0)
    return output


def setup_page(page: Any) -> None:
    page.set_content(
        """
<!doctype html>
<meta charset="utf-8">
<style>
html, body { margin: 0; padding: 0; background: white; }
#scroller { width: 800px; height: 720px; overflow: auto; contain: strict; }
#content {
  box-sizing: border-box;
  width: 776px;
  margin: 0;
  padding: 6px 12px;
  font: 16px/18px monospace;
  white-space: pre-wrap;
  overflow-wrap: anywhere;
}
</style>
<div id="scroller"><pre id="content"></pre></div>
"""
    )


def create_blob(page: Any, payload_bytes: int) -> dict[str, Any]:
    return page.evaluate(
        """
async ({ payloadBytes }) => {
  const encoder = new TextEncoder();
  const pattern = encoder.encode('👨‍👩‍👧‍👦👍🏽🚀 ');
  const chunkBytes = 1024 * 1024;
  const chunk = new Uint8Array(chunkBytes);
  for (let index = 0; index < pattern.length; ++index) chunk[index] = pattern[index];
  let filled = pattern.length;
  while (filled < chunk.length) {
    const copy = Math.min(filled, chunk.length - filled);
    chunk.set(chunk.subarray(0, copy), filled);
    filled += copy;
  }
  const fullChunks = Math.floor(payloadBytes / chunkBytes);
  const remainder = payloadBytes % chunkBytes;
  const parts = Array(fullChunks).fill(chunk);
  if (remainder) parts.push(chunk.subarray(0, remainder));
  window.__payloadBlob = new Blob(parts, { type: 'text/plain;charset=utf-8' });
  window.__payloadBytes = payloadBytes;
  return { blob_bytes: window.__payloadBlob.size, pattern_bytes: pattern.length };
}
""",
        {"payloadBytes": payload_bytes},
    )


def setup_virtualized(page: Any, payload_bytes: int) -> dict[str, Any]:
    blob = create_blob(page, payload_bytes)
    page.evaluate(
        """
() => {
  document.getElementById('content').textContent = '';
  window.__renderVirtualSlice = async (start, length) => {
    const started = performance.now();
    const text = await window.__payloadBlob.slice(start, start + length).text();
    const content = document.getElementById('content');
    content.textContent = text;
    void content.offsetHeight;
    await new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)));
    return {
      milliseconds: performance.now() - started,
      rendered_utf16_units: text.length,
      rendered_height: content.offsetHeight,
    };
  };
}
"""
    )
    return blob


def setup_native(page: Any, payload_bytes: int) -> dict[str, Any]:
    blob = create_blob(page, payload_bytes)
    native = page.evaluate(
        """
async () => {
  const started = performance.now();
  const text = await window.__payloadBlob.text();
  const content = document.getElementById('content');
  content.textContent = text;
  const scrollHeight = document.getElementById('scroller').scrollHeight;
  await new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)));
  window.__nativeText = text;
  window.__scrollNative = async fraction => {
    const scroller = document.getElementById('scroller');
    const began = performance.now();
    scroller.scrollTop = Math.floor(Math.max(0, scroller.scrollHeight - scroller.clientHeight) * fraction);
    void content.offsetHeight;
    await new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)));
    return performance.now() - began;
  };
  return {
    setup_milliseconds: performance.now() - started,
    decoded_utf16_units: text.length,
    scroll_height: scrollHeight,
  };
}
"""
    )
    return {**blob, **native}


def browser_worker(
    browser_name: str,
    mode: str,
    payload_bytes: int,
    query_count: int,
    slice_bytes: int,
    output_queue: Any,
) -> None:
    monitor: PeakPssMonitor | None = None
    browser = None
    try:
        root_pid = os.getpid()
        with sync_playwright() as playwright:
            baseline_pss = process_tree_pss_bytes(root_pid)
            monitor = PeakPssMonitor(root_pid)
            monitor.start()
            browser_type = getattr(playwright, browser_name)
            launch_args = ["--js-flags=--expose-gc"] if browser_name == "chromium" else []
            browser = browser_type.launch(headless=True, args=launch_args)
            context = browser.new_context(viewport={"width": 800, "height": 720})
            page = context.new_page()
            setup_page(page)

            setup_started = time.perf_counter()
            if mode == "virtualized":
                setup_metrics = setup_virtualized(page, payload_bytes)
            elif mode == "native-dom":
                setup_metrics = setup_native(page, payload_bytes)
            else:
                raise ValueError(f"unknown mode: {mode}")
            setup_wall_seconds = time.perf_counter() - setup_started
            page.evaluate("() => { if (globalThis.gc) globalThis.gc(); }")
            time.sleep(0.25)
            resident_after_setup = process_tree_pss_bytes(root_pid)

            query_ms: list[float] = []
            query_details: list[dict[str, Any]] = []
            if mode == "virtualized":
                offsets = deterministic_offsets(payload_bytes, slice_bytes, query_count)
                for offset in offsets:
                    detail = page.evaluate(
                        "([start, length]) => window.__renderVirtualSlice(start, length)",
                        [offset, slice_bytes],
                    )
                    query_ms.append(float(detail["milliseconds"]))
                    query_details.append({"byte_offset": offset, **detail})
            else:
                offsets = deterministic_offsets(1_000_000, 1, query_count)
                for offset in offsets:
                    fraction = offset / 1_000_000.0
                    milliseconds = float(
                        page.evaluate("fraction => window.__scrollNative(fraction)", fraction)
                    )
                    query_ms.append(milliseconds)
                    query_details.append(
                        {"scroll_fraction": fraction, "milliseconds": milliseconds}
                    )

            resident_after_queries = process_tree_pss_bytes(root_pid)
            monitor.stop()
            result = {
                "status": "success",
                "browser": browser_name,
                "browser_version": browser.version,
                "mode": mode,
                "payload_bytes": payload_bytes,
                "query_count": query_count,
                "slice_bytes": slice_bytes if mode == "virtualized" else None,
                "setup_wall_seconds": setup_wall_seconds,
                "setup_metrics": setup_metrics,
                "query_milliseconds_p50": percentile(query_ms, 50.0),
                "query_milliseconds_p95": percentile(query_ms, 95.0),
                "query_milliseconds_p99": percentile(query_ms, 99.0),
                "query_milliseconds_max": max(query_ms),
                "query_milliseconds_mean": statistics.fmean(query_ms),
                "harness_baseline_pss_mb": baseline_pss / 1_000_000,
                "process_tree_pss_mb_after_setup": resident_after_setup / 1_000_000,
                "process_tree_pss_mb_after_queries": resident_after_queries / 1_000_000,
                "process_tree_peak_pss_mb": monitor.peak_bytes / 1_000_000,
                "incremental_peak_pss_mb": max(0, monitor.peak_bytes - baseline_pss) / 1_000_000,
                "query_details": query_details,
            }
            context.close()
            browser.close()
            browser = None
            output_queue.put(result)
    except BaseException as exc:  # noqa: BLE001 - worker must serialize all failures
        if monitor is not None:
            monitor.stop()
        if browser is not None:
            try:
                browser.close()
            except BaseException:
                pass
        output_queue.put(
            {
                "status": "error",
                "browser": browser_name,
                "mode": mode,
                "payload_bytes": payload_bytes,
                "error": f"{type(exc).__name__}: {exc}",
                "traceback": traceback.format_exc(),
            }
        )


def run_case(
    browser_name: str,
    mode: str,
    payload_bytes: int,
    query_count: int,
    slice_bytes: int,
    timeout_seconds: int,
) -> dict[str, Any]:
    context = mp.get_context("spawn")
    output_queue = context.Queue()
    process = context.Process(
        target=browser_worker,
        args=(
            browser_name,
            mode,
            payload_bytes,
            query_count,
            slice_bytes,
            output_queue,
        ),
    )
    process.start()
    process.join(timeout_seconds)
    if process.is_alive():
        process.terminate()
        process.join(15)
        return {
            "status": "timeout",
            "browser": browser_name,
            "mode": mode,
            "payload_bytes": payload_bytes,
            "timeout_seconds": timeout_seconds,
        }
    try:
        return output_queue.get(timeout=5)
    except queue.Empty:
        return {
            "status": "error",
            "browser": browser_name,
            "mode": mode,
            "payload_bytes": payload_bytes,
            "error": f"worker exited with code {process.exitcode} without a result",
        }


def zevryon_summary(report: dict[str, Any]) -> dict[str, Any]:
    layout = report["layout_window"]
    comparison = layout["comparison"]
    baseline = layout["baseline_layout"]
    return {
        "payload_bytes": int(report["giant_record_bytes"]),
        "layout_model": "deterministic average-advance fragments, not browser font shaping",
        "baseline_full_scan_seconds": float(comparison["baseline_seconds"]),
        "baseline_full_scan_peak_pss_mb": baseline.get("peak_pss_mb"),
        "baseline_source_bytes_read": int(comparison["baseline_source_bytes_read"]),
        "checkpoint_query_count": int(comparison["checkpoint_query_count"]),
        "checkpoint_seconds_p50": float(comparison["checkpoint_window_seconds_p50"]),
        "checkpoint_seconds_p95": float(comparison["checkpoint_window_seconds_p95"]),
        "checkpoint_seconds_p99": float(comparison["checkpoint_window_seconds_p99"]),
        "checkpoint_seconds_max": float(comparison["checkpoint_window_seconds_max"]),
        "checkpoint_peak_pss_mb_max": comparison.get("checkpoint_peak_pss_mb_max"),
        "checkpoint_source_bytes_read_max": int(
            comparison["checkpoint_source_bytes_read_max"]
        ),
        "checkpoint_physical_bytes": int(comparison["checkpoint_physical_bytes"]),
        "checkpoint_overhead_ratio": float(comparison["checkpoint_overhead_ratio"]),
        "speedup_x_p50": float(comparison["speedup_x_p50"]),
        "speedup_x_p95": float(comparison["speedup_x_p95"]),
        "source_read_reduction_x_worst": float(
            comparison["source_read_reduction_x_worst"]
        ),
        "checkpoint_build_seconds": float(comparison["checkpoint_build_seconds"]),
        "zero_payload_data_loss": bool(layout["zero_payload_data_loss"]),
    }


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Compare Zevryon giant-record access with Chromium and Firefox"
    )
    parser.add_argument("--zevryon-report", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--payload-bytes", type=int, default=64 * 1024 * 1024)
    parser.add_argument("--query-count", type=int, default=21)
    parser.add_argument("--virtual-slice-bytes", type=int, default=128 * 1024)
    parser.add_argument("--virtual-timeout-seconds", type=int, default=180)
    parser.add_argument("--native-timeout-seconds", type=int, default=420)
    args = parser.parse_args()

    if (
        args.payload_bytes <= 0
        or args.query_count <= 0
        or args.virtual_slice_bytes <= 0
        or args.virtual_timeout_seconds <= 0
        or args.native_timeout_seconds <= 0
    ):
        parser.error("benchmark arguments must be positive")

    zevryon_report = json.loads(args.zevryon_report.read_text(encoding="utf-8"))
    cases: list[dict[str, Any]] = []
    for browser_name in ("chromium", "firefox"):
        cases.append(
            run_case(
                browser_name,
                "virtualized",
                args.payload_bytes,
                args.query_count,
                args.virtual_slice_bytes,
                args.virtual_timeout_seconds,
            )
        )
        cases.append(
            run_case(
                browser_name,
                "native-dom",
                args.payload_bytes,
                args.query_count,
                args.virtual_slice_bytes,
                args.native_timeout_seconds,
            )
        )

    virtual_failures = [
        case
        for case in cases
        if case["mode"] == "virtualized" and case["status"] != "success"
    ]
    report = {
        "schema": "zevryon.competitor.giant-document.v1",
        "host": {
            "platform": os.uname().sysname,
            "kernel": os.uname().release,
            "logical_cpus": os.cpu_count(),
        },
        "payload_bytes": args.payload_bytes,
        "query_count": args.query_count,
        "virtual_slice_bytes": args.virtual_slice_bytes,
        "scope_notes": [
            "Zevryon checkpoint timings include CLI process start, store open, checkpoint open, bounded read, and JSON serialization.",
            "Browser query timings are steady-context page timings after browser and payload setup.",
            "Browser process memory is incremental process-tree PSS above the Python+Playwright harness baseline.",
            "Native DOM uses real browser text layout; Zevryon currently uses deterministic average-advance fragments.",
            "Virtualized browser mode uses a 64 MiB Blob and renders only a bounded 128 KiB slice.",
        ],
        "zevryon": zevryon_summary(zevryon_report),
        "browser_cases": cases,
        "all_virtualized_cases_succeeded": not virtual_failures,
    }
    text = json.dumps(report, indent=2, sort_keys=True) + "\n"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(text, encoding="utf-8")
    print(text, end="")
    return 1 if virtual_failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
