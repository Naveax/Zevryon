#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys

SMOKE_OPERATIONS = 50_000
BELOW_CERTIFICATION = 9_999_999
CERTIFICATION_MINIMUM = 10_000_000


class TestFailure(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise TestFailure(message)


def run(command: list[str], expected_exit: int) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        command,
        text=True,
        encoding="utf-8",
        errors="replace",
        capture_output=True,
        check=False,
        timeout=60.0,
    )
    require(
        result.returncode == expected_exit,
        f"exit mismatch: expected {expected_exit}, got {result.returncode}; stdout={result.stdout!r}; stderr={result.stderr!r}",
    )
    return result


def load_report(path: Path) -> dict[str, object]:
    require(path.is_file(), f"report was not written: {path}")
    value = json.loads(path.read_text(encoding="utf-8"))
    require(isinstance(value, dict), "report is not a JSON object")
    return value


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    args = parser.parse_args()

    try:
        probe = args.probe.resolve()
        require(probe.is_file(), f"probe not found: {probe}")
        work = args.work_dir.resolve()
        if work.exists():
            shutil.rmtree(work)
        work.mkdir(parents=True)

        smoke_path = work / "smoke.json"
        result = run(
            [
                str(probe),
                "--operations",
                str(SMOKE_OPERATIONS),
                "--output",
                str(smoke_path),
            ],
            0,
        )
        report = load_report(smoke_path)
        require(report.get("schema") == "zevryon.m8.mixed-mutation.v2", "schema mismatch")
        require(report.get("authority") == "m8-sequence-mixed-mutation-integrity-v2", "authority mismatch")
        require(report.get("mode") == "smoke", "smoke mode mismatch")
        require(report.get("operations_requested") == SMOKE_OPERATIONS, "requested operation count mismatch")
        require(report.get("operations_completed") == SMOKE_OPERATIONS, "completed operation count mismatch")
        require(report.get("certification_minimum_operations") == CERTIFICATION_MINIMUM, "certification threshold drift")
        require(report.get("certification_threshold_met") is False, "50k smoke met certification threshold")
        require(report.get("certification_eligible") is False, "50k smoke was incorrectly certification-eligible")
        require(report.get("gate_passed") is True, "smoke integrity gate did not pass")
        require(report.get("failure_reason") is None, "passing smoke emitted a failure reason")
        require(report.get("logical_order_mismatches") == 0, "logical-order mismatch was hidden")
        require(report.get("integrity_mismatches") == 0, "integrity mismatch was hidden")
        require(
            report.get("live_logical_order_digest") == report.get("oracle_logical_order_digest"),
            "live/oracle digest mismatch",
        )
        counts = report.get("operation_counts")
        require(isinstance(counts, dict), "operation counts missing")
        required_actions = {"insert", "erase", "move", "update_height", "update_summary"}
        require(set(counts) == required_actions, "operation count domain mismatch")
        require(all(type(counts[name]) is int and counts[name] > 0 for name in required_actions), "smoke did not exercise every mutation class")
        require(sum(counts.values()) == SMOKE_OPERATIONS, "operation counts do not sum to completed operations")
        require(report.get("operation_count_sum_matches") is True, "count-sum receipt missing")
        require(report.get("all_mutation_classes_exercised") is True, "mutation-class coverage receipt missing")
        require(type(report.get("verification_checkpoints")) is int and report["verification_checkpoints"] > 0, "no integrity checkpoint receipt")
        require(result.stdout == smoke_path.read_text(encoding="utf-8"), "stdout/report receipt drift")

        failure_path = work / "terminal-failure.json"
        failed = run(
            [
                str(probe),
                "--operations",
                "1",
                "--output",
                str(failure_path),
            ],
            2,
        )
        failure = load_report(failure_path)
        require(failure.get("operations_requested") == 1, "failure requested-count drift")
        require(failure.get("operations_completed") == 1, "failure completed-count receipt missing")
        require(failure.get("gate_passed") is False, "terminal failure was reported as pass")
        require(failure.get("all_mutation_classes_exercised") is False, "terminal failure hid missing mutation classes")
        require(type(failure.get("integrity_mismatches")) is int and failure["integrity_mismatches"] > 0, "terminal failure lacked mismatch receipt")
        require(isinstance(failure.get("failure_reason"), str) and failure["failure_reason"], "terminal failure reason missing")
        require(failed.stdout == failure_path.read_text(encoding="utf-8"), "failure stdout/report receipt drift")

        rejected = run(
            [
                str(probe),
                "--certification",
                "--operations",
                str(BELOW_CERTIFICATION),
            ],
            1,
        )
        require(
            "requires at least 10000000 operations" in rejected.stderr,
            "below-threshold certification rejection diagnostic drifted",
        )
        require("certification_eligible" not in rejected.stdout, "invalid configuration emitted a misleading evidence report")
    except (TestFailure, OSError, json.JSONDecodeError, subprocess.SubprocessError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1

    print("Zevryon M8 mixed-mutation smoke authority tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
