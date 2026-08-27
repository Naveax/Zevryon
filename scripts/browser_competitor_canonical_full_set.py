#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys
from typing import Mapping

from browser_competitor_benchmark_evidence import HARNESS_SCHEMA
from browser_competitor_registry import CANONICAL_KEYS


class CanonicalFullSetInvalid(ValueError):
    pass


def validate_canonical_full_set_report(report: Mapping[str, object]) -> None:
    if report.get("schema") != HARNESS_SCHEMA:
        raise CanonicalFullSetInvalid("benchmark report schema mismatch")

    requested = report.get("requested_competitors")
    if requested != list(CANONICAL_KEYS):
        raise CanonicalFullSetInvalid(
            "benchmark report did not request the exact canonical competitor set"
        )
    if report.get("all_requested_cases_succeeded") is not True:
        raise CanonicalFullSetInvalid("one or more canonical benchmark cases failed")

    coverage_by_mode = report.get("leadership_coverage_by_mode")
    if not isinstance(coverage_by_mode, Mapping):
        raise CanonicalFullSetInvalid("benchmark report lacks per-mode leadership coverage")

    for mode in ("virtualized", "native-dom"):
        coverage = coverage_by_mode.get(mode)
        if not isinstance(coverage, Mapping):
            raise CanonicalFullSetInvalid(f"benchmark report lacks {mode} coverage")
        if coverage.get("canonical_requested") != list(CANONICAL_KEYS):
            raise CanonicalFullSetInvalid(
                f"{mode} coverage canonical request set drifted"
            )
        if coverage.get("canonical_missing") != []:
            raise CanonicalFullSetInvalid(f"{mode} coverage has missing competitors")
        if coverage.get("canonical_unsuccessful") != []:
            raise CanonicalFullSetInvalid(f"{mode} coverage has unsuccessful competitors")
        if coverage.get("comparison_mismatches") != {}:
            raise CanonicalFullSetInvalid(f"{mode} comparison identity drifted")
        if coverage.get("full_set_coverage") is not True:
            raise CanonicalFullSetInvalid(f"{mode} full-set coverage did not pass")
        if coverage.get("comparable_full_set") is not True:
            raise CanonicalFullSetInvalid(f"{mode} comparable full set did not pass")
        if coverage.get("leadership_evidence_gate_passed") is not True:
            raise CanonicalFullSetInvalid(f"{mode} leadership evidence gate did not pass")
        if coverage.get("leadership_metric_gate_evaluated") is not False:
            raise CanonicalFullSetInvalid(
                f"{mode} report unexpectedly evaluated the metric gate"
            )
        if coverage.get("leadership_eligible") is not False:
            raise CanonicalFullSetInvalid(
                f"{mode} report claimed leadership before metric evaluation"
            )

    if report.get("leadership_metric_gate_evaluated") is not False:
        raise CanonicalFullSetInvalid(
            "canonical full-set evidence cannot evaluate the metric gate"
        )
    if report.get("leadership_eligible") is not False:
        raise CanonicalFullSetInvalid(
            "canonical full-set evidence cannot claim leadership"
        )


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Run the exact canonical M7 competitor set and fail closed unless both "
            "benchmark modes produce complete comparable evidence"
        )
    )
    parser.add_argument("--zevryon-report", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--payload-bytes", type=int, default=64 * 1024 * 1024)
    parser.add_argument("--query-count", type=int, default=21)
    parser.add_argument("--virtual-slice-bytes", type=int, default=128 * 1024)
    parser.add_argument("--virtual-timeout-seconds", type=int, default=180)
    parser.add_argument("--native-timeout-seconds", type=int, default=420)
    args = parser.parse_args()

    benchmark = Path(__file__).with_name("browser_competitor_benchmark.py")
    command = [
        sys.executable,
        str(benchmark),
        "--zevryon-report",
        str(args.zevryon_report),
        "--output",
        str(args.output),
        "--payload-bytes",
        str(args.payload_bytes),
        "--query-count",
        str(args.query_count),
        "--virtual-slice-bytes",
        str(args.virtual_slice_bytes),
        "--virtual-timeout-seconds",
        str(args.virtual_timeout_seconds),
        "--native-timeout-seconds",
        str(args.native_timeout_seconds),
    ]
    for competitor in CANONICAL_KEYS:
        command.extend(["--competitor", competitor])

    completed = subprocess.run(command, check=False)
    if not args.output.is_file():
        return completed.returncode if completed.returncode != 0 else 1

    try:
        report = json.loads(args.output.read_text(encoding="utf-8"))
        if not isinstance(report, dict):
            raise CanonicalFullSetInvalid("benchmark report is not a JSON object")
        validate_canonical_full_set_report(report)
    except (OSError, json.JSONDecodeError, CanonicalFullSetInvalid) as exc:
        print(f"canonical full-set evidence rejected: {exc}", file=sys.stderr)
        return 1

    if completed.returncode != 0:
        print(
            "canonical full-set benchmark returned non-zero despite a passing report",
            file=sys.stderr,
        )
        return 1
    print("canonical full-set evidence gate: passed", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
