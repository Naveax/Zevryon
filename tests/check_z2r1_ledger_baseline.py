#!/usr/bin/env python3
"""Validate Z2R-1 ledger benchmark artifacts against measured promotion gates."""

from __future__ import annotations

import json
import math
import pathlib
import sys
from typing import Any

SCHEMA = "zevryon.ledger-performance.v1"
MIN_OPERATIONS_PER_SECOND = 5_000_000.0
MAX_RUST_P50_RATIO = 2.50
MAX_RUST_P99_RATIO = 3.00
MAX_SHADOW_P50_RATIO = 3.50
MAX_SHADOW_P99_RATIO = 4.00


def metric(report: dict[str, Any], implementation: str, field: str) -> float:
    value = float(report[implementation][field])
    if not math.isfinite(value) or value <= 0.0:
        raise ValueError(f"{implementation}.{field} must be finite and positive")
    return value


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: check_z2r1_ledger_baseline.py REPORT.json", file=sys.stderr)
        return 2

    path = pathlib.Path(sys.argv[1])
    try:
        report = json.loads(path.read_text(encoding="utf-8"))
        if report.get("schema") != SCHEMA:
            raise ValueError(f"unexpected schema: {report.get('schema')!r}")

        cpp_p50 = metric(report, "cpp", "p50_ns_per_operation")
        cpp_p99 = metric(report, "cpp", "p99_ns_per_operation")
        rust_p50 = metric(report, "rust", "p50_ns_per_operation")
        rust_p99 = metric(report, "rust", "p99_ns_per_operation")
        shadow_p50 = metric(report, "shadow", "p50_ns_per_operation")
        shadow_p99 = metric(report, "shadow", "p99_ns_per_operation")

        ratios = {
            "rust_p50": rust_p50 / cpp_p50,
            "rust_p99": rust_p99 / cpp_p99,
            "shadow_p50": shadow_p50 / cpp_p50,
            "shadow_p99": shadow_p99 / cpp_p99,
        }
        limits = {
            "rust_p50": MAX_RUST_P50_RATIO,
            "rust_p99": MAX_RUST_P99_RATIO,
            "shadow_p50": MAX_SHADOW_P50_RATIO,
            "shadow_p99": MAX_SHADOW_P99_RATIO,
        }

        failures: list[str] = []
        for name, ratio in ratios.items():
            if ratio > limits[name]:
                failures.append(f"{name} ratio {ratio:.3f} exceeds {limits[name]:.3f}")

        for implementation in ("cpp", "rust", "shadow"):
            throughput = metric(report, implementation, "operations_per_second")
            if throughput < MIN_OPERATIONS_PER_SECOND:
                failures.append(
                    f"{implementation} throughput {throughput:.0f} ops/s is below "
                    f"{MIN_OPERATIONS_PER_SECOND:.0f} ops/s"
                )

        print(
            "Z2R-1 baseline: "
            f"Rust p50={ratios['rust_p50']:.3f}x p99={ratios['rust_p99']:.3f}x; "
            f"shadow p50={ratios['shadow_p50']:.3f}x "
            f"p99={ratios['shadow_p99']:.3f}x"
        )
        if failures:
            for failure in failures:
                print(f"FAILED: {failure}", file=sys.stderr)
            return 1
        print("Z2R-1 measured performance gates passed")
        return 0
    except (OSError, ValueError, KeyError, TypeError, json.JSONDecodeError) as error:
        print(f"FAILED: invalid benchmark report: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
