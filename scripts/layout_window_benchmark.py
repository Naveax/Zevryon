#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
from pathlib import Path

from massivedoc_benchmark import run_measured


def main() -> int:
    parser = argparse.ArgumentParser(description="Benchmark the bounded M3A layout window")
    parser.add_argument("--binary", type=Path, default=Path("build/zevryon-massivedoc"))
    parser.add_argument("--store", type=Path, required=True)
    parser.add_argument("--viewport-width", type=int, default=800)
    parser.add_argument("--viewport-height", type=int, default=720)
    parser.add_argument("--overscan", type=int, default=720)
    parser.add_argument("--max-fragments", type=int, default=512)
    parser.add_argument("--cache-mb", type=int, default=8)
    parser.add_argument("--expected-record", type=int)
    parser.add_argument("--minimum-source-bytes", type=int, default=0)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    if (
        args.viewport_width <= 0
        or args.viewport_height <= 0
        or args.overscan < 0
        or args.max_fragments <= 0
        or args.cache_mb <= 0
        or args.minimum_source_bytes < 0
    ):
        parser.error("layout benchmark arguments must be positive")

    arena_before = run_measured([str(args.binary), "arena-stats", str(args.store)])
    total_height_q8 = int(arena_before["result"]["total_height_q8"])
    middle_y_px = total_height_q8 // 512
    layout = run_measured(
        [
            str(args.binary),
            "layout-window",
            str(args.store),
            str(middle_y_px),
            str(args.viewport_width),
            str(args.viewport_height),
            str(args.overscan),
            str(args.max_fragments),
            str(args.cache_mb),
        ]
    )
    arena_after = run_measured([str(args.binary), "arena-stats", str(args.store)])
    verified_after_layout = run_measured([str(args.binary), "verify", str(args.store)])

    result = layout["result"]
    if result.get("operation") != "layout-window":
        raise RuntimeError("layout command did not identify its operation")
    window = result["layout"]
    fragments = window["fragments"]
    if not fragments:
        raise RuntimeError("layout window returned no fragments")
    if int(window["fragment_count"]) != len(fragments):
        raise RuntimeError("layout fragment count does not match payload")
    if len(fragments) > args.max_fragments:
        raise RuntimeError("layout window exceeded max-fragments")
    if int(window["cache_bytes"]) > args.cache_mb * 1_000_000:
        raise RuntimeError("layout cache exceeded decimal-MB budget")
    if int(window["source_bytes_read"]) < args.minimum_source_bytes:
        raise RuntimeError("layout scanner did not stream the expected source volume")
    if args.expected_record is not None and any(
        int(fragment["record_index"]) != args.expected_record for fragment in fragments
    ):
        raise RuntimeError("middle layout escaped the expected giant record")
    if any(int(fragment["source_end"]) <= int(fragment["source_start"]) for fragment in fragments):
        raise RuntimeError("layout fragment failed to advance through source bytes")
    if not verified_after_layout["result"].get("ok"):
        raise RuntimeError("payload verification failed after layout metadata updates")

    report = {
        "schema": "zevryon.massivedoc.layout-window-benchmark.v1",
        "viewport_width_px": args.viewport_width,
        "viewport_height_px": args.viewport_height,
        "overscan_px": args.overscan,
        "max_fragments": args.max_fragments,
        "cache_budget_bytes": args.cache_mb * 1_000_000,
        "expected_record": args.expected_record,
        "minimum_source_bytes": args.minimum_source_bytes,
        "middle_scroll_y_px": middle_y_px,
        "arena_before": arena_before,
        "layout": layout,
        "arena_after": arena_after,
        "verify_after_layout": verified_after_layout,
        "bounded_fragment_materialization": True,
        "bounded_cache": True,
        "zero_payload_data_loss": True,
    }
    text = json.dumps(report, indent=2, sort_keys=True) + "\n"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(text, encoding="utf-8")
    print(text, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
