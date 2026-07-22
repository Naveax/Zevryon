#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
from pathlib import Path

from massivedoc_benchmark import run_measured


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Benchmark M3A full scan against ZENITH sparse checkpoints"
    )
    parser.add_argument("--binary", type=Path, default=Path("build/zevryon-massivedoc"))
    parser.add_argument("--store", type=Path, required=True)
    parser.add_argument("--viewport-width", type=int, default=800)
    parser.add_argument("--viewport-height", type=int, default=720)
    parser.add_argument("--overscan", type=int, default=720)
    parser.add_argument("--max-fragments", type=int, default=512)
    parser.add_argument("--cache-mb", type=int, default=8)
    parser.add_argument("--checkpoint-stride-kib", type=int, default=64)
    parser.add_argument("--checkpoint-max-source-bytes", type=int, default=2 * 1024 * 1024)
    parser.add_argument("--checkpoint-max-overhead-ratio", type=float, default=0.005)
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
        or args.checkpoint_stride_kib <= 0
        or args.checkpoint_max_source_bytes <= 0
        or not 0.0 < args.checkpoint_max_overhead_ratio < 1.0
        or args.minimum_source_bytes < 0
    ):
        parser.error("layout benchmark arguments are outside their valid range")

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

    checkpoint_build = None
    checkpoint_window = None
    comparison = None
    if args.expected_record is not None:
        checkpoint_build = run_measured(
            [
                str(args.binary),
                "checkpoint-build",
                str(args.store),
                str(args.expected_record),
                str(args.viewport_width),
                str(args.checkpoint_stride_kib),
            ]
        )
        checkpoint = checkpoint_build["result"]["checkpoint"]
        measured_height_q8 = int(checkpoint["measured_height_q8"])
        source_bytes = int(checkpoint["source_bytes"])
        physical_bytes = int(checkpoint["physical_bytes"])
        if source_bytes <= 0:
            raise RuntimeError("checkpoint reported an empty source record")
        if physical_bytes / source_bytes > args.checkpoint_max_overhead_ratio:
            raise RuntimeError("checkpoint exceeded physical-overhead gate")

        measured_height_px = measured_height_q8 // 256
        query_height_px = args.viewport_height + 2 * args.overscan
        local_start_px = max(0, measured_height_px // 2 - args.overscan)
        checkpoint_window = run_measured(
            [
                str(args.binary),
                "checkpoint-window",
                str(args.store),
                str(args.expected_record),
                str(local_start_px),
                str(args.viewport_width),
                str(query_height_px),
                str(args.max_fragments),
                str(args.checkpoint_stride_kib),
            ]
        )
        accelerated = checkpoint_window["result"]
        accelerated_fragments = accelerated["fragments"]
        accelerated_read = int(accelerated["source_bytes_read"])
        if int(accelerated["record_index"]) != args.expected_record:
            raise RuntimeError("checkpoint window opened the wrong logical record")
        if not accelerated_fragments:
            raise RuntimeError("checkpoint window returned no fragments")
        if len(accelerated_fragments) > args.max_fragments:
            raise RuntimeError("checkpoint window exceeded max-fragments")
        if accelerated_read > args.checkpoint_max_source_bytes:
            raise RuntimeError("checkpoint window exceeded bounded source-read gate")
        if int(accelerated["checkpoint_source_offset"]) <= 0:
            raise RuntimeError("checkpoint window did not seek inside the giant record")
        if any(
            int(fragment["source_end"]) <= int(fragment["source_start"])
            for fragment in accelerated_fragments
        ):
            raise RuntimeError("checkpoint fragment failed to advance through source bytes")

        baseline_seconds = float(layout["seconds"])
        accelerated_seconds = float(checkpoint_window["seconds"])
        baseline_read = int(window["source_bytes_read"])
        comparison = {
            "baseline_seconds": baseline_seconds,
            "checkpoint_window_seconds": accelerated_seconds,
            "speedup_x": baseline_seconds / accelerated_seconds if accelerated_seconds else None,
            "baseline_source_bytes_read": baseline_read,
            "checkpoint_source_bytes_read": accelerated_read,
            "source_read_reduction_x": baseline_read / accelerated_read if accelerated_read else None,
            "checkpoint_build_seconds": float(checkpoint_build["seconds"]),
            "checkpoint_physical_bytes": physical_bytes,
            "checkpoint_overhead_ratio": physical_bytes / source_bytes,
            "queries_to_amortize_build": (
                float(checkpoint_build["seconds"])
                / max(1e-12, baseline_seconds - accelerated_seconds)
                if baseline_seconds > accelerated_seconds
                else None
            ),
        }

    verified_after_layout = run_measured([str(args.binary), "verify", str(args.store)])
    if not verified_after_layout["result"].get("ok"):
        raise RuntimeError("payload verification failed after layout metadata updates")

    report = {
        "schema": "zevryon.massivedoc.layout-window-benchmark.v2",
        "viewport_width_px": args.viewport_width,
        "viewport_height_px": args.viewport_height,
        "overscan_px": args.overscan,
        "max_fragments": args.max_fragments,
        "cache_budget_bytes": args.cache_mb * 1_000_000,
        "checkpoint_stride_bytes": args.checkpoint_stride_kib * 1024,
        "checkpoint_max_source_bytes": args.checkpoint_max_source_bytes,
        "checkpoint_max_overhead_ratio": args.checkpoint_max_overhead_ratio,
        "expected_record": args.expected_record,
        "minimum_source_bytes": args.minimum_source_bytes,
        "middle_scroll_y_px": middle_y_px,
        "arena_before": arena_before,
        "baseline_layout": layout,
        "checkpoint_build": checkpoint_build,
        "checkpoint_window": checkpoint_window,
        "comparison": comparison,
        "arena_after": arena_after,
        "verify_after_layout": verified_after_layout,
        "bounded_fragment_materialization": True,
        "bounded_cache": True,
        "bounded_checkpoint_random_access": checkpoint_window is not None,
        "zero_payload_data_loss": True,
    }
    text = json.dumps(report, indent=2, sort_keys=True) + "\n"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(text, encoding="utf-8")
    print(text, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
