#!/usr/bin/env python3
from __future__ import annotations

import argparse
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from zevryon_platform.benchmark_metadata import capture_benchmark_metadata
from zevryon_platform.frame_latency_evidence import (
    evaluate_frame_latencies,
    parse_frame_samples,
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Evaluate physical-device frame latency samples against the canonical Zevryon device profile."
    )
    parser.add_argument("--samples", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument(
        "--require-pass",
        action="store_true",
        help="return a non-zero exit code unless all physical frame-latency gates pass",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    samples = parse_frame_samples(args.samples.read_text(encoding="utf-8"))
    machine = capture_benchmark_metadata()
    evidence = evaluate_frame_latencies(samples, machine)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(evidence.to_json() + "\n", encoding="utf-8")
    print(
        f"device={machine.device_class.value} samples={evidence.summary.sample_count} "
        f"p99_ms={evidence.summary.p99_ms:.6f} "
        f"limit_ms={evidence.p99_threshold_ms:.6f} "
        f"certified={str(evidence.certified).lower()}"
    )
    if args.require_pass and not evidence.certified:
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
