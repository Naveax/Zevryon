#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
from pathlib import Path

from massivedoc_benchmark import run_measured


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Certify stateful ZENITH random and adjacent hot-scroll performance"
    )
    parser.add_argument(
        "--massivedoc-binary", type=Path, default=Path("build/zevryon-massivedoc")
    )
    parser.add_argument(
        "--hot-binary", type=Path, default=Path("build/zevryon-zenith-hot")
    )
    parser.add_argument("--store", type=Path, required=True)
    parser.add_argument("--layout-report", type=Path, required=True)
    parser.add_argument("--expected-record", type=int, required=True)
    parser.add_argument("--viewport-width", type=int, default=800)
    parser.add_argument("--viewport-height", type=int, default=720)
    parser.add_argument("--overscan", type=int, default=720)
    parser.add_argument("--max-fragments", type=int, default=512)
    parser.add_argument("--queries", type=int, default=257)
    parser.add_argument("--stride-kib", type=int, default=16)
    parser.add_argument("--random-radius-px", type=int, default=1_000_000)
    parser.add_argument("--random-p95-ms", type=float, default=2.0)
    parser.add_argument("--adjacent-p95-ms", type=float, default=0.5)
    parser.add_argument("--max-source-bytes", type=int, default=64 * 1024)
    parser.add_argument("--minimum-adjacent-zero-read-ratio", type=float, default=0.95)
    parser.add_argument("--maximum-index-overhead-ratio", type=float, default=0.002)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    if (
        args.viewport_width <= 0
        or args.viewport_height <= 0
        or args.overscan < 0
        or args.max_fragments <= 0
        or args.queries < 3
        or args.stride_kib <= 0
        or args.random_radius_px < 0
        or args.random_p95_ms <= 0
        or args.adjacent_p95_ms <= 0
        or args.max_source_bytes <= 0
        or not 0.0 <= args.minimum_adjacent_zero_read_ratio <= 1.0
        or not 0.0 < args.maximum_index_overhead_ratio < 1.0
    ):
        parser.error("hot-scroll benchmark arguments are outside their valid range")

    layout_report = json.loads(args.layout_report.read_text(encoding="utf-8"))
    baseline_fragments = layout_report["baseline_layout"]["result"]["layout"][
        "fragments"
    ]
    if not baseline_fragments:
        raise RuntimeError("baseline report contains no fragments")
    center = baseline_fragments[len(baseline_fragments) // 2]
    if int(center["record_index"]) != args.expected_record:
        raise RuntimeError("baseline center is not inside the expected giant record")
    center_scroll_y_px = int(center["y_q8"]) // 256

    checkpoint_build = run_measured(
        [
            str(args.massivedoc_binary),
            "checkpoint-build",
            str(args.store),
            str(args.expected_record),
            str(args.viewport_width),
            str(args.stride_kib),
        ]
    )
    checkpoint = checkpoint_build["result"]["checkpoint"]
    source_bytes = int(checkpoint["source_bytes"])
    index_bytes = int(checkpoint["physical_bytes"])
    if source_bytes <= 0:
        raise RuntimeError("hot-scroll checkpoint source is empty")
    index_overhead_ratio = index_bytes / source_bytes
    if index_overhead_ratio > args.maximum_index_overhead_ratio:
        raise RuntimeError("hot-scroll checkpoint exceeded index-overhead gate")

    hot = run_measured(
        [
            str(args.hot_binary),
            str(args.store),
            str(center_scroll_y_px),
            str(args.viewport_width),
            str(args.viewport_height),
            str(args.queries),
            str(args.overscan),
            str(args.max_fragments),
            str(args.stride_kib),
            str(args.random_radius_px),
        ]
    )
    result = hot["result"]
    if result.get("operation") != "zenith-hot-scroll":
        raise RuntimeError("hot-scroll executable did not identify its operation")
    if int(result["queries"]) != args.queries:
        raise RuntimeError("hot-scroll executable returned the wrong query count")
    if int(result["stride_bytes"]) != args.stride_kib * 1024:
        raise RuntimeError("hot-scroll executable used the wrong checkpoint stride")

    random_profile = result["random"]
    adjacent_profile = result["adjacent"]
    random_session = result["random_session"]
    adjacent_session = result["adjacent_session"]

    if float(random_profile["p95_ms"]) > args.random_p95_ms:
        raise RuntimeError("random hot-scroll P95 exceeded hard latency gate")
    if float(adjacent_profile["p95_ms"]) > args.adjacent_p95_ms:
        raise RuntimeError("adjacent hot-scroll P95 exceeded hard latency gate")
    if int(random_profile["maximum_source_bytes_read"]) > args.max_source_bytes:
        raise RuntimeError("random hot-scroll exceeded one source-window gate")
    if int(adjacent_profile["maximum_source_bytes_read"]) > args.max_source_bytes:
        raise RuntimeError("adjacent hot-scroll exceeded one source-window gate")

    adjacent_zero_ratio = (
        int(adjacent_profile["zero_source_read_queries"]) / args.queries
    )
    if adjacent_zero_ratio < args.minimum_adjacent_zero_read_ratio:
        raise RuntimeError("adjacent source-window reuse ratio is below gate")
    if int(random_profile["checkpoint_cache_misses"]) != 0:
        raise RuntimeError("warmed random session reparsed a checkpoint")
    if int(adjacent_profile["checkpoint_cache_misses"]) != 0:
        raise RuntimeError("warmed adjacent session reparsed a checkpoint")
    if int(random_session["checkpoint_cache_peak_bytes"]) > int(
        result["checkpoint_cache_budget_bytes"]
    ):
        raise RuntimeError("random checkpoint cache exceeded byte budget")
    if int(adjacent_session["checkpoint_cache_peak_bytes"]) > int(
        result["checkpoint_cache_budget_bytes"]
    ):
        raise RuntimeError("adjacent checkpoint cache exceeded byte budget")
    if int(random_session["source_window_cache_peak_bytes"]) > int(
        result["source_window_cache_budget_bytes"]
    ):
        raise RuntimeError("random source-window cache exceeded byte budget")
    if int(adjacent_session["source_window_cache_peak_bytes"]) > int(
        result["source_window_cache_budget_bytes"]
    ):
        raise RuntimeError("adjacent source-window cache exceeded byte budget")

    verified = run_measured(
        [str(args.massivedoc_binary), "verify", str(args.store)]
    )
    if not verified["result"].get("ok"):
        raise RuntimeError("payload verification failed after hot-scroll benchmark")

    report = {
        "schema": "zevryon.zenith.hot-scroll.v1",
        "expected_record": args.expected_record,
        "center_scroll_y_px": center_scroll_y_px,
        "viewport_width_px": args.viewport_width,
        "viewport_height_px": args.viewport_height,
        "overscan_px": args.overscan,
        "queries_per_profile": args.queries,
        "stride_bytes": args.stride_kib * 1024,
        "random_radius_px": args.random_radius_px,
        "random_p95_gate_ms": args.random_p95_ms,
        "adjacent_p95_gate_ms": args.adjacent_p95_ms,
        "maximum_source_bytes_gate": args.max_source_bytes,
        "minimum_adjacent_zero_read_ratio": args.minimum_adjacent_zero_read_ratio,
        "checkpoint_build": checkpoint_build,
        "checkpoint_index_bytes": index_bytes,
        "checkpoint_index_overhead_ratio": index_overhead_ratio,
        "hot_scroll": hot,
        "adjacent_zero_source_read_ratio": adjacent_zero_ratio,
        "verify_after_hot_scroll": verified,
        "bounded_checkpoint_cache": True,
        "bounded_source_window_cache": True,
        "zero_payload_data_loss": True,
    }
    text = json.dumps(report, indent=2, sort_keys=True) + "\n"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(text, encoding="utf-8")
    print(text, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
