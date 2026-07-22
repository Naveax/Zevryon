#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
from pathlib import Path

from massivedoc_benchmark import run_measured


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Benchmark the global ZENITH checkpoint-aware layout path"
    )
    parser.add_argument("--binary", type=Path, default=Path("build/zevryon-zenith-layout"))
    parser.add_argument("--store", type=Path, required=True)
    parser.add_argument("--layout-report", type=Path, required=True)
    parser.add_argument("--expected-record", type=int, required=True)
    parser.add_argument("--viewport-width", type=int, default=800)
    parser.add_argument("--viewport-height", type=int, default=720)
    parser.add_argument("--overscan", type=int, default=720)
    parser.add_argument("--max-fragments", type=int, default=512)
    parser.add_argument("--stride-kib", type=int, default=64)
    parser.add_argument("--max-source-bytes", type=int, default=2 * 1024 * 1024)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    if (
        args.viewport_width <= 0
        or args.viewport_height <= 0
        or args.overscan < 0
        or args.max_fragments <= 0
        or args.stride_kib <= 0
        or args.max_source_bytes <= 0
    ):
        parser.error("ZENITH global benchmark arguments are invalid")

    layout_report = json.loads(args.layout_report.read_text(encoding="utf-8"))
    baseline_fragments = layout_report["baseline_layout"]["result"]["layout"]["fragments"]
    if not baseline_fragments:
        raise RuntimeError("baseline layout report has no fragments")
    center = baseline_fragments[len(baseline_fragments) // 2]
    if int(center["record_index"]) != args.expected_record:
        raise RuntimeError("baseline center fragment is not inside the expected giant record")
    scroll_y_px = int(center["y_q8"]) // 256

    measured = run_measured(
        [
            str(args.binary),
            str(args.store),
            str(scroll_y_px),
            str(args.viewport_width),
            str(args.viewport_height),
            str(args.overscan),
            str(args.max_fragments),
            str(args.stride_kib),
        ]
    )
    result = measured["result"]
    if result.get("operation") != "zenith-layout-window":
        raise RuntimeError("ZENITH executable did not identify its operation")
    window = result["layout"]
    fragments = window["fragments"]
    if not window.get("checkpoint_accelerated"):
        raise RuntimeError("global layout did not use persistent checkpoints")
    if int(window["checkpoint_hits"]) < 2:
        raise RuntimeError("global layout did not use checkpoints in both passes")
    if int(window["source_bytes_read"]) > args.max_source_bytes:
        raise RuntimeError("global layout exceeded bounded source-read gate")
    if not fragments:
        raise RuntimeError("global layout returned no fragments")
    if len(fragments) > args.max_fragments:
        raise RuntimeError("global layout exceeded max-fragments")
    if any(int(fragment["record_index"]) != args.expected_record for fragment in fragments):
        raise RuntimeError("global layout escaped the expected giant record")

    report = {
        "schema": "zevryon.zenith.global-layout.v1",
        "expected_record": args.expected_record,
        "scroll_y_px": scroll_y_px,
        "max_source_bytes": args.max_source_bytes,
        "measurement": measured,
        "checkpoint_accelerated": True,
        "bounded_global_random_access": True,
    }
    text = json.dumps(report, indent=2, sort_keys=True) + "\n"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(text, encoding="utf-8")
    print(text, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
