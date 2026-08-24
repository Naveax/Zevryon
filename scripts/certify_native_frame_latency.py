#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from zevryon_platform.benchmark_metadata import capture_benchmark_metadata
from zevryon_platform.frame_latency_evidence import (
    evaluate_frame_latencies,
    parse_frame_samples,
)
from zevryon_platform.native_frame_certification import (
    NativeFrameProbeSummary,
    build_native_frame_certification,
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run the native ZenithTabRuntime frame probe and produce physical frame-latency certification evidence."
    )
    parser.add_argument("--probe", required=True, type=Path)
    parser.add_argument("--store", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--samples-output", type=Path)
    parser.add_argument("--samples", type=int, default=2000)
    parser.add_argument("--warmup", type=int, default=120)
    parser.add_argument("--width-px", type=int, default=1440)
    parser.add_argument("--height-px", type=int, default=900)
    parser.add_argument("--overscan-px", type=int, default=720)
    parser.add_argument("--max-fragments", type=int, default=512)
    parser.add_argument("--step-px", type=int, default=18)
    parser.add_argument("--timeout-seconds", type=int, default=900)
    parser.add_argument(
        "--require-pass",
        action="store_true",
        help="return non-zero unless the full native physical frame gate passes",
    )
    return parser.parse_args()


def _positive(name: str, value: int, *, allow_zero: bool = False) -> int:
    minimum = 0 if allow_zero else 1
    if value < minimum:
        raise ValueError(f"{name} must be >= {minimum}")
    return value


def _sample_output_path(args: argparse.Namespace) -> Path:
    if args.samples_output is not None:
        return args.samples_output
    return args.output.with_name(args.output.stem + ".samples.txt")


def _last_json_line(stdout: str) -> str:
    lines = [line.strip() for line in stdout.splitlines() if line.strip()]
    if not lines:
        raise ValueError("native frame probe produced no stdout summary")
    json.loads(lines[-1])
    return lines[-1]


def main() -> int:
    args = parse_args()
    try:
        _positive("samples", args.samples)
        _positive("warmup", args.warmup, allow_zero=True)
        _positive("width-px", args.width_px)
        _positive("height-px", args.height_px)
        _positive("overscan-px", args.overscan_px, allow_zero=True)
        _positive("max-fragments", args.max_fragments)
        _positive("step-px", args.step_px)
        _positive("timeout-seconds", args.timeout_seconds)
    except ValueError as exc:
        print(str(exc), file=sys.stderr)
        return 2

    machine = capture_benchmark_metadata()
    profile = machine.device_class.value
    sample_output = _sample_output_path(args)
    sample_output.parent.mkdir(parents=True, exist_ok=True)
    args.output.parent.mkdir(parents=True, exist_ok=True)

    command = [
        str(args.probe),
        str(args.store),
        profile,
        str(sample_output),
        str(args.samples),
        str(args.warmup),
        str(args.width_px),
        str(args.height_px),
        str(args.overscan_px),
        str(args.max_fragments),
        str(args.step_px),
    ]
    try:
        completed = subprocess.run(
            command,
            check=False,
            capture_output=True,
            text=True,
            timeout=args.timeout_seconds,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        print(f"native frame probe launch failed: {exc}", file=sys.stderr)
        return 1

    if completed.returncode != 0:
        if completed.stdout:
            print(completed.stdout.rstrip(), file=sys.stderr)
        if completed.stderr:
            print(completed.stderr.rstrip(), file=sys.stderr)
        print(
            f"native frame probe failed with exit code {completed.returncode}",
            file=sys.stderr,
        )
        return 1

    try:
        probe = NativeFrameProbeSummary.from_json_line(_last_json_line(completed.stdout))
        samples = parse_frame_samples(sample_output.read_text(encoding="utf-8"))
        frame_latency = evaluate_frame_latencies(samples, machine)
        certification = build_native_frame_certification(
            probe,
            frame_latency,
            requested_samples=args.samples,
            requested_warmup=args.warmup,
        )
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"native frame evidence assembly failed: {exc}", file=sys.stderr)
        return 1

    args.output.write_text(certification.to_json() + "\n", encoding="utf-8")
    print(
        f"device={profile} samples={frame_latency.summary.sample_count} "
        f"p99_ms={frame_latency.summary.p99_ms:.6f} "
        f"limit_ms={frame_latency.p99_threshold_ms:.6f} "
        f"native_certified={str(certification.certified).lower()}"
    )
    if args.require_pass and not certification.certified:
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
