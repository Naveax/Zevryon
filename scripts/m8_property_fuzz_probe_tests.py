#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys

SMOKE_CASES = 8
CERTIFICATION_MINIMUM = 10_000
SEED = 8_675_309
EXPECTED_DOMAINS = ["unicode", "serializer", "index", "sequence"]


class TestFailure(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise TestFailure(message)


def run(command: list[str], expected_exit: int, timeout: float = 180.0) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        command,
        text=True,
        encoding="utf-8",
        errors="replace",
        capture_output=True,
        check=False,
        timeout=timeout,
    )
    require(
        result.returncode == expected_exit,
        f"exit mismatch: expected {expected_exit}, got {result.returncode}; stdout={result.stdout!r}; stderr={result.stderr!r}",
    )
    return result


def load_report(path: Path) -> dict[str, object]:
    require(path.is_file(), f"report missing: {path}")
    value = json.loads(path.read_text(encoding="utf-8"))
    require(isinstance(value, dict), "property-fuzz report is not an object")
    return value


def validate_smoke(report: dict[str, object]) -> dict[str, str]:
    require(report.get("schema") == "zevryon.m8.property-fuzz.v1", "schema mismatch")
    require(report.get("authority") == "m8-four-domain-property-fuzz-v1", "authority mismatch")
    require(report.get("mode") == "smoke", "mode mismatch")
    require(report.get("seed") == SEED, "seed receipt mismatch")
    require(report.get("cases_requested_per_domain") == SMOKE_CASES, "case-count receipt mismatch")
    require(
        report.get("certification_minimum_cases_per_domain") == CERTIFICATION_MINIMUM,
        "certification threshold drift",
    )
    require(report.get("certification_threshold_met") is False, "smoke met certification threshold")
    require(report.get("certification_eligible") is False, "smoke was incorrectly certification eligible")
    require(report.get("gate_passed") is True, "smoke gate did not pass")

    domains = report.get("domains")
    require(isinstance(domains, list) and len(domains) == 4, "four domain receipts are required")
    names = [item.get("name") for item in domains if isinstance(item, dict)]
    require(names == EXPECTED_DOMAINS, f"domain order/set mismatch: {names!r}")
    digests: dict[str, str] = {}
    for item in domains:
        require(isinstance(item, dict), "domain receipt is not an object")
        name = item.get("name")
        require(isinstance(name, str), "domain name missing")
        require(item.get("cases_completed") == SMOKE_CASES, f"{name} did not complete every case")
        require(item.get("failures") == 0, f"{name} reported a failure")
        require(item.get("failure_case") is None, f"{name} emitted a failure case on pass")
        require(item.get("failure_seed") is None, f"{name} emitted a failure seed on pass")
        require(item.get("failure_reason") is None, f"{name} emitted a failure reason on pass")
        digest = item.get("digest")
        require(isinstance(digest, str) and len(digest) == 16 and digest != "0000000000000000", f"{name} digest missing")
        digests[name] = digest
    return digests


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    args = parser.parse_args()

    try:
        probe = args.probe.resolve()
        require(probe.is_file(), f"probe not found: {probe}")
        root = args.work_dir.resolve()
        if root.exists():
            shutil.rmtree(root)
        root.mkdir(parents=True)

        first_work = root / "first"
        first_report = root / "first-report.json"
        first = run(
            [
                str(probe),
                "--work-dir", str(first_work),
                "--output", str(first_report),
                "--cases", str(SMOKE_CASES),
                "--seed", str(SEED),
            ],
            0,
        )
        first_value = load_report(first_report)
        first_digests = validate_smoke(first_value)
        require(first.stdout == first_report.read_text(encoding="utf-8"), "stdout/report receipt drift")

        replay_work = root / "replay"
        replay_report = root / "replay-report.json"
        replay = run(
            [
                str(probe),
                "--work-dir", str(replay_work),
                "--output", str(replay_report),
                "--cases", str(SMOKE_CASES),
                "--seed", str(SEED),
            ],
            0,
        )
        replay_value = load_report(replay_report)
        replay_digests = validate_smoke(replay_value)
        require(replay_digests == first_digests, "same-seed replay changed one or more domain digests")
        require(replay.stdout == replay_report.read_text(encoding="utf-8"), "replay stdout/report drift")

        rejected = run(
            [
                str(probe),
                "--work-dir", str(root / "below-certification"),
                "--certification",
                "--cases", str(CERTIFICATION_MINIMUM - 1),
            ],
            1,
        )
        require(
            "requires at least 10000 cases per domain" in rejected.stderr,
            "below-threshold certification rejection diagnostic drifted",
        )
        require("certification_eligible" not in rejected.stdout, "invalid certification emitted evidence")
    except (TestFailure, OSError, json.JSONDecodeError, subprocess.SubprocessError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1

    print("Zevryon M8 four-domain property-fuzz smoke tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
