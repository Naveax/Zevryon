#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys

SMOKE_SECONDS = 3
SMOKE_PAYLOAD_BYTES = 256 * 1024
BELOW_CERTIFICATION_SECONDS = 86_399
CERTIFICATION_MINIMUM_SECONDS = 86_400


class TestFailure(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise TestFailure(message)


def run(command: list[str], expected_exit: int, timeout: float = 45.0) -> subprocess.CompletedProcess[str]:
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


def parse_jsonl(text: str) -> list[dict[str, object]]:
    events: list[dict[str, object]] = []
    for line_number, line in enumerate(text.splitlines(), start=1):
        require(line.strip() != "", f"blank JSONL line at {line_number}")
        value = json.loads(line)
        require(isinstance(value, dict), f"JSONL event {line_number} is not an object")
        events.append(value)
    require(events, "soak emitted no JSONL events")
    return events


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    args = parser.parse_args()

    try:
        probe = args.probe.resolve()
        require(probe.is_file(), f"probe not found: {probe}")
        outer = args.work_dir.resolve()
        if outer.exists():
            shutil.rmtree(outer)
        outer.mkdir(parents=True)

        probe_work = outer / "probe-work"
        report_path = probe_work / "evidence" / "smoke.jsonl"
        result = run(
            [
                str(probe),
                "--work-dir",
                str(probe_work),
                "--output",
                str(report_path),
                "--duration-seconds",
                str(SMOKE_SECONDS),
                "--payload-bytes",
                str(SMOKE_PAYLOAD_BYTES),
            ],
            0,
        )
        require(report_path.is_file(), "soak smoke event log was not written")
        file_text = report_path.read_text(encoding="utf-8")
        require(result.stdout == file_text, "soak stdout/event-log receipt drift")
        events = parse_jsonl(file_text)
        require(len(events) >= 4, "soak smoke produced too few continuity events")

        for event in events:
            require(event.get("schema") == "zevryon.m8.soak-event.v1", "soak event schema mismatch")
            require(event.get("authority") == "m8-continuous-dual-mode-soak-v1", "soak authority mismatch")

        start = events[0]
        complete = events[-1]
        checkpoints = events[1:-1]
        require(start.get("event") == "start", "first soak event is not start")
        require(complete.get("event") == "complete", "last soak event is not complete")
        require(all(event.get("event") == "checkpoint" for event in checkpoints), "non-checkpoint event appeared inside active soak")
        require(len(checkpoints) >= 2, "3-second smoke did not preserve at least two continuity checkpoints")

        pid = start.get("process_id")
        require(type(pid) is int and pid > 0, "start process id missing")
        require(all(event.get("process_id") == pid for event in events), "soak events crossed process boundaries")
        require(start.get("mode") == "smoke", "smoke start mode drifted")
        require(start.get("duration_seconds_requested") == SMOKE_SECONDS, "smoke duration receipt drifted")
        require(start.get("certification_minimum_seconds") == CERTIFICATION_MINIMUM_SECONDS, "24h threshold drifted")
        require(start.get("payload_bytes") == SMOKE_PAYLOAD_BYTES, "smoke payload receipt drifted")
        require(isinstance(start.get("payload_sha256"), str) and len(start["payload_sha256"]) == 64, "payload SHA receipt missing")
        checkpoint_interval = start.get("checkpoint_interval_ms")
        require(type(checkpoint_interval) is int and checkpoint_interval == 1000, "smoke checkpoint interval drifted")
        memory_interval = start.get("memory_sample_interval_ms")
        require(type(memory_interval) is int and memory_interval == 250, "smoke memory interval drifted")

        last_elapsed = 0
        last_virtual = 0
        last_native = 0
        for expected_ordinal, checkpoint in enumerate(checkpoints, start=1):
            require(checkpoint.get("ordinal") == expected_ordinal, "checkpoint ordinal drifted")
            elapsed = checkpoint.get("elapsed_ms")
            gap = checkpoint.get("checkpoint_gap_ms")
            virtualized = checkpoint.get("virtualized_queries")
            native = checkpoint.get("native_queries")
            require(type(elapsed) is int and elapsed > last_elapsed, "checkpoint elapsed time did not increase")
            require(type(gap) is int and 0 < gap <= checkpoint_interval * 2, "checkpoint continuity gap exceeded frozen bound")
            require(type(checkpoint.get("max_checkpoint_gap_ms")) is int and checkpoint["max_checkpoint_gap_ms"] <= checkpoint_interval * 2, "max checkpoint gap receipt exceeded bound")
            require(type(virtualized) is int and virtualized > last_virtual, "virtualized query count did not progress")
            require(type(native) is int and native > last_native, "native query count did not progress")
            require(isinstance(checkpoint.get("rolling_digest"), str) and checkpoint["rolling_digest"] != "0000000000000000", "checkpoint digest missing")
            require(type(checkpoint.get("memory_samples")) is int and checkpoint["memory_samples"] > 0, "checkpoint has no memory receipt")
            require(type(checkpoint.get("current_rss_bytes")) is int and checkpoint["current_rss_bytes"] > 0, "checkpoint current RSS missing")
            require(type(checkpoint.get("peak_rss_bytes")) is int and checkpoint["peak_rss_bytes"] >= checkpoint["current_rss_bytes"], "checkpoint peak RSS below current RSS")
            last_elapsed = elapsed
            last_virtual = virtualized
            last_native = native

        require(complete.get("mode") == "smoke", "terminal smoke mode drifted")
        require(type(complete.get("elapsed_ms")) is int and complete["elapsed_ms"] >= SMOKE_SECONDS * 1000, "smoke ended before target duration")
        require(complete.get("duration_target_met") is True, "duration target receipt failed")
        require(complete.get("checkpoint_coverage_met") is True, "checkpoint coverage receipt failed")
        require(complete.get("checkpoint_gap_within_limit") is True, "checkpoint gap gate failed")
        require(type(complete.get("checkpoint_count")) is int and complete["checkpoint_count"] == len(checkpoints), "terminal checkpoint count drifted")
        require(type(complete.get("minimum_checkpoint_count")) is int and complete["minimum_checkpoint_count"] >= 2, "terminal minimum checkpoint receipt drifted")
        require(type(complete.get("virtualized_queries")) is int and complete["virtualized_queries"] >= last_virtual, "terminal virtualized count regressed")
        require(type(complete.get("native_queries")) is int and complete["native_queries"] >= last_native, "terminal native count regressed")
        require(isinstance(complete.get("rolling_digest"), str) and complete["rolling_digest"] != "0000000000000000", "terminal rolling digest missing")
        require(type(complete.get("memory_samples")) is int and complete["memory_samples"] > 0, "terminal memory evidence missing")
        require(complete.get("memory_snapshot_failures") == 0, "memory snapshot failure was hidden")
        require(complete.get("query_failures") == 0, "query failure was hidden")
        require(type(complete.get("peak_rss_bytes")) is int and complete["peak_rss_bytes"] > 0, "terminal peak RSS missing")
        require(type(complete.get("current_rss_bytes")) is int and complete["peak_rss_bytes"] >= complete["current_rss_bytes"], "terminal peak RSS below current RSS")
        require(complete.get("certification_eligible") is False, "3-second smoke was incorrectly 24h-certification eligible")
        require(complete.get("failure_reason") is None, "passing smoke emitted failure reason")
        require(complete.get("gate_passed") is True, "soak smoke gate failed")

        rejected = run(
            [
                str(probe),
                "--work-dir",
                str(outer / "rejected-certification"),
                "--certification",
                "--duration-seconds",
                str(BELOW_CERTIFICATION_SECONDS),
                "--payload-bytes",
                str(SMOKE_PAYLOAD_BYTES),
            ],
            1,
        )
        require(
            "requires at least 86400 seconds" in rejected.stderr,
            "below-threshold soak certification diagnostic drifted",
        )
        require(rejected.stdout == "", "invalid soak certification emitted misleading evidence")
    except (TestFailure, OSError, json.JSONDecodeError, subprocess.SubprocessError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1

    print("Zevryon M8 continuous-soak smoke authority tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
