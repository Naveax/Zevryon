#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import os
import statistics
import subprocess
import time
from pathlib import Path
from typing import Any

import psutil


def canonical_json(value: Any) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False)


def sha256_json(value: Any) -> str:
    return hashlib.sha256(canonical_json(value).encode("utf-8")).hexdigest()


def semantic_view(result: dict[str, Any]) -> dict[str, Any]:
    view = dict(result)
    view.pop("elapsed_ms", None)
    view.pop("shadow", None)
    return view


def run_process(
    binary: Path,
    logical_bytes: int,
    rounds: int,
    *,
    fault: str | None = None,
) -> dict[str, Any]:
    env = os.environ.copy()
    if fault is None:
        env.pop("ZEVRYON_UTF8_SHADOW_FAULT", None)
    else:
        env["ZEVRYON_UTF8_SHADOW_FAULT"] = fault

    command = [
        str(binary),
        "--logical-bytes",
        str(logical_bytes),
        "--rounds",
        str(rounds),
    ]
    started = time.perf_counter()
    process = psutil.Popen(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
        env=env,
    )
    peak_rss = 0
    while process.poll() is None:
        try:
            peak_rss = max(peak_rss, int(process.memory_info().rss))
        except (psutil.Error, ProcessLookupError):
            pass
        time.sleep(0.002)

    stdout, stderr = process.communicate()
    elapsed = time.perf_counter() - started
    try:
        peak_rss = max(peak_rss, int(process.memory_info().rss))
    except (psutil.Error, ProcessLookupError):
        pass

    if process.returncode != 0:
        raise RuntimeError(
            f"workload failed ({process.returncode}): {' '.join(command)}\n"
            f"stdout:\n{stdout}\nstderr:\n{stderr}"
        )

    lines = [line for line in stdout.splitlines() if line.strip()]
    if len(lines) != 1:
        raise RuntimeError(f"expected one JSON line, received {len(lines)}")
    result = json.loads(lines[0])
    if result.get("schema") != "zevryon.z2r3du.unicode-workload.v1":
        raise RuntimeError("unexpected workload schema")

    return {
        "command": command,
        "fault": fault or "none",
        "seconds": elapsed,
        "peak_rss_bytes": peak_rss,
        "stderr": stderr,
        "result": result,
        "semantic_sha256": sha256_json(semantic_view(result)),
    }


def distribution(samples: list[dict[str, Any]], field: str) -> dict[str, float]:
    values = sorted(float(sample[field]) for sample in samples)
    return {
        "minimum": values[0],
        "p50": statistics.median(values),
        "p95": values[min(len(values) - 1, int((len(values) - 1) * 0.95 + 0.999999))],
        "p99": values[min(len(values) - 1, int((len(values) - 1) * 0.99 + 0.999999))],
        "maximum": values[-1],
        "mean": statistics.fmean(values),
    }


def build_report(args: argparse.Namespace) -> dict[str, Any]:
    baseline_binary = Path(args.baseline_binary).resolve()
    shadow_binary = Path(args.shadow_binary).resolve()
    if not baseline_binary.is_file():
        raise FileNotFoundError(baseline_binary)
    if not shadow_binary.is_file():
        raise FileNotFoundError(shadow_binary)

    baseline: list[dict[str, Any]] = []
    shadow: list[dict[str, Any]] = []
    for index in range(args.samples):
        order = (
            [("baseline", baseline_binary), ("shadow", shadow_binary)]
            if index % 2 == 0
            else [("shadow", shadow_binary), ("baseline", baseline_binary)]
        )
        for mode, binary in order:
            sample = run_process(
                binary,
                args.logical_bytes,
                args.rounds,
            )
            (baseline if mode == "baseline" else shadow).append(sample)

    baseline_semantics = {sample["semantic_sha256"] for sample in baseline}
    shadow_semantics = {sample["semantic_sha256"] for sample in shadow}
    if len(baseline_semantics) != 1 or len(shadow_semantics) != 1:
        raise RuntimeError("workload semantics changed between repeated samples")
    if baseline_semantics != shadow_semantics:
        raise RuntimeError("baseline/shadow semantic divergence")

    baseline_result = baseline[0]["result"]
    shadow_result = shadow[0]["result"]
    if baseline_result["shadow"]["enabled"]:
        raise RuntimeError("baseline unexpectedly enabled the Rust Unicode shadow")
    if not shadow_result["shadow"]["enabled"]:
        raise RuntimeError("strict shadow build did not enable the Rust Unicode shadow")
    if not shadow_result["shadow"]["healthy"]:
        raise RuntimeError("strict shadow build reported unhealthy telemetry")
    if int(shadow_result["shadow"]["mismatches"]) != 0:
        raise RuntimeError("strict shadow build reported a mismatch")

    faults: dict[str, dict[str, Any]] = {}
    if args.fault_binary:
        fault_binary = Path(args.fault_binary).resolve()
        if not fault_binary.is_file():
            raise FileNotFoundError(fault_binary)
        for fault in ("output", "error", "state", "reset"):
            faults[fault] = run_process(
                fault_binary,
                args.logical_bytes,
                1,
                fault=fault,
            )

    report: dict[str, Any] = {
        "schema": "zevryon.z2r3du.paired-report.v1",
        "platform": args.platform,
        "logical_bytes": args.logical_bytes,
        "rounds": args.rounds,
        "samples": args.samples,
        "baseline": baseline,
        "shadow": shadow,
        "faults": faults,
        "semantic_sha256": next(iter(baseline_semantics)),
        "seconds": {
            "baseline": distribution(baseline, "seconds"),
            "shadow": distribution(shadow, "seconds"),
        },
        "peak_rss_bytes": {
            "baseline": distribution(baseline, "peak_rss_bytes"),
            "shadow": distribution(shadow, "peak_rss_bytes"),
        },
    }
    report["report_sha256"] = sha256_json(report)
    return report


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--baseline-binary", required=True)
    parser.add_argument("--shadow-binary", required=True)
    parser.add_argument("--fault-binary")
    parser.add_argument("--platform", required=True, choices=("linux", "windows", "macos"))
    parser.add_argument("--logical-bytes", type=int, default=16 * 1024 * 1024)
    parser.add_argument("--rounds", type=int, default=2)
    parser.add_argument("--samples", type=int, default=3)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    if args.logical_bytes < 4096 or args.logical_bytes > 128 * 1024 * 1024:
        parser.error("--logical-bytes outside certification bounds")
    if args.rounds < 1 or args.rounds > 16:
        parser.error("--rounds outside certification bounds")
    if args.samples < 3 or args.samples > 15:
        parser.error("--samples must be between 3 and 15")
    if args.platform != "linux" and args.fault_binary:
        parser.error("--fault-binary is only accepted for the Linux certification")
    return args


def main() -> int:
    args = parse_args()
    report = build_report(args)
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(json.dumps({
        "platform": report["platform"],
        "semantic_sha256": report["semantic_sha256"],
        "report_sha256": report["report_sha256"],
        "faults": sorted(report["faults"]),
    }, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
