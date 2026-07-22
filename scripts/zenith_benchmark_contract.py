#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import platform
import sys
from pathlib import Path
from typing import Any

SCHEMA = "zevryon.benchmark.v1"
STAGE_MODES = {"cold", "warm", "hot"}


class BenchmarkContractError(ValueError):
    pass


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise BenchmarkContractError(message)


def _stage(
    *,
    name: str,
    mode: str,
    samples: int,
    p50_ms: float,
    p95_ms: float,
    p99_ms: float,
    maximum_ms: float,
    physical_read_bytes: int,
    peak_pss_bytes: int | None,
) -> dict[str, Any]:
    return {
        "name": name,
        "mode": mode,
        "samples": samples,
        "latency_ms": {
            "p50": p50_ms,
            "p95": p95_ms,
            "p99": p99_ms,
            "maximum": maximum_ms,
        },
        "physical_read_bytes": physical_read_bytes,
        "peak_pss_bytes": peak_pss_bytes,
    }


def build_giant_record_report(
    *,
    massivedoc: dict[str, Any],
    global_layout: dict[str, Any],
    hot_scroll: dict[str, Any],
    commit_sha: str,
    compiler: str,
    build_type: str,
    runner_os: str,
) -> dict[str, Any]:
    payload_sha256 = massivedoc.get("payload_sha256")
    _require(isinstance(payload_sha256, str) and len(payload_sha256) == 64, "missing payload SHA-256")

    global_measurement = global_layout["measurement"]
    global_result = global_measurement["result"]["layout"]
    global_ms = float(global_measurement["seconds"]) * 1000.0

    hot_measurement = hot_scroll["hot_scroll"]
    hot_result = hot_measurement["result"]
    random_profile = hot_result["random"]
    adjacent_profile = hot_result["adjacent"]
    random_session = hot_result["random_session"]
    adjacent_session = hot_result["adjacent_session"]

    checkpoint_budget = int(hot_result["checkpoint_cache_budget_bytes"])
    source_budget = int(hot_result["source_window_cache_budget_bytes"])
    checkpoint_peak = max(
        int(random_session["checkpoint_cache_peak_bytes"]),
        int(adjacent_session["checkpoint_cache_peak_bytes"]),
    )
    source_peak = max(
        int(random_session["source_window_cache_peak_bytes"]),
        int(adjacent_session["source_window_cache_peak_bytes"]),
    )

    verify_result = hot_scroll["verify_after_hot_scroll"]["result"]
    correctness = {
        "massivedoc_zero_data_loss": massivedoc.get("zero_data_loss") is True,
        "hot_scroll_zero_payload_data_loss": hot_scroll.get("zero_payload_data_loss") is True,
        "post_run_payload_verification": verify_result.get("ok") is True,
        "global_checkpoint_accelerated": global_layout.get("checkpoint_accelerated") is True,
        "bounded_global_random_access": global_layout.get("bounded_global_random_access") is True,
        "bounded_checkpoint_cache": hot_scroll.get("bounded_checkpoint_cache") is True,
        "bounded_source_window_cache": hot_scroll.get("bounded_source_window_cache") is True,
    }

    report = {
        "schema": SCHEMA,
        "benchmark": "giant-record-viewport",
        "workload": {
            "id": "giant-64m-v1",
            "logical_bytes": int(massivedoc["logical_bytes"]),
            "logical_records": int(massivedoc["logical_records"]),
            "giant_record_bytes": int(massivedoc["giant_record_bytes"]),
            "payload_sha256": payload_sha256,
        },
        "build": {
            "commit_sha": commit_sha,
            "compiler": compiler,
            "build_type": build_type,
        },
        "environment": {
            "runner_os": runner_os,
            "platform": platform.platform(),
            "machine": platform.machine(),
            "python": platform.python_version(),
        },
        "stages": [
            _stage(
                name="global_checkpoint_layout",
                mode="warm",
                samples=1,
                p50_ms=global_ms,
                p95_ms=global_ms,
                p99_ms=global_ms,
                maximum_ms=global_ms,
                physical_read_bytes=int(global_result["source_bytes_read"]),
                peak_pss_bytes=global_measurement.get("peak_pss_bytes"),
            ),
            _stage(
                name="random_hot_scroll",
                mode="hot",
                samples=int(hot_result["queries"]),
                p50_ms=float(random_profile["p50_ms"]),
                p95_ms=float(random_profile["p95_ms"]),
                p99_ms=float(random_profile["p99_ms"]),
                maximum_ms=float(random_profile["maximum_ms"]),
                physical_read_bytes=int(random_profile["total_source_bytes_read"]),
                peak_pss_bytes=hot_measurement.get("peak_pss_bytes"),
            ),
            _stage(
                name="adjacent_hot_scroll",
                mode="hot",
                samples=int(hot_result["queries"]),
                p50_ms=float(adjacent_profile["p50_ms"]),
                p95_ms=float(adjacent_profile["p95_ms"]),
                p99_ms=float(adjacent_profile["p99_ms"]),
                maximum_ms=float(adjacent_profile["maximum_ms"]),
                physical_read_bytes=int(adjacent_profile["total_source_bytes_read"]),
                peak_pss_bytes=hot_measurement.get("peak_pss_bytes"),
            ),
        ],
        "resources": {
            "checkpoint_index": {
                "hard_limit_bytes": checkpoint_budget,
                "current_bytes": max(
                    int(random_session["checkpoint_cache_bytes"]),
                    int(adjacent_session["checkpoint_cache_bytes"]),
                ),
                "peak_bytes": checkpoint_peak,
                "cache_hits": int(random_session["checkpoint_cache_hits"])
                + int(adjacent_session["checkpoint_cache_hits"]),
                "cache_misses": int(random_session["checkpoint_cache_misses"])
                + int(adjacent_session["checkpoint_cache_misses"]),
                "evictions": int(random_session["checkpoint_cache_evictions"])
                + int(adjacent_session["checkpoint_cache_evictions"]),
            },
            "source_window": {
                "hard_limit_bytes": source_budget,
                "current_bytes": max(
                    int(random_session["source_window_cache_bytes"]),
                    int(adjacent_session["source_window_cache_bytes"]),
                ),
                "peak_bytes": source_peak,
                "cache_hits": int(random_session["source_window_cache_hits"])
                + int(adjacent_session["source_window_cache_hits"]),
                "cache_misses": int(random_session["source_window_cache_misses"])
                + int(adjacent_session["source_window_cache_misses"]),
                "evictions": int(random_session["source_window_cache_evictions"])
                + int(adjacent_session["source_window_cache_evictions"]),
            },
        },
        "correctness": correctness,
        "gates": {
            "random_p95_ms_max": float(hot_scroll["random_p95_gate_ms"]),
            "adjacent_p95_ms_max": float(hot_scroll["adjacent_p95_gate_ms"]),
            "maximum_source_read_bytes": int(hot_scroll["maximum_source_bytes_gate"]),
            "checkpoint_overhead_ratio_max": 0.002,
            "payload_data_loss_bytes_max": 0,
        },
    }
    validate_report(report)
    return report


def validate_report(report: dict[str, Any]) -> None:
    _require(report.get("schema") == SCHEMA, "unexpected benchmark schema")
    _require(isinstance(report.get("benchmark"), str) and report["benchmark"], "benchmark name required")

    workload = report.get("workload")
    _require(isinstance(workload, dict), "workload object required")
    _require(int(workload.get("logical_bytes", 0)) > 0, "logical bytes must be positive")
    _require(int(workload.get("logical_records", 0)) > 0, "logical records must be positive")
    _require(int(workload.get("giant_record_bytes", 0)) > 0, "giant record must be positive")
    payload_sha256 = workload.get("payload_sha256")
    _require(isinstance(payload_sha256, str) and len(payload_sha256) == 64, "payload SHA-256 required")

    build = report.get("build")
    _require(isinstance(build, dict), "build object required")
    commit_sha = build.get("commit_sha")
    _require(isinstance(commit_sha, str) and commit_sha and commit_sha != "unknown", "commit SHA required")
    _require(isinstance(build.get("compiler"), str) and build["compiler"], "compiler required")
    _require(isinstance(build.get("build_type"), str) and build["build_type"], "build type required")

    environment = report.get("environment")
    _require(isinstance(environment, dict), "environment object required")
    for key in ("runner_os", "platform", "machine", "python"):
        _require(isinstance(environment.get(key), str) and environment[key], f"environment {key} required")

    stages = report.get("stages")
    _require(isinstance(stages, list) and stages, "at least one benchmark stage required")
    names: set[str] = set()
    for stage in stages:
        name = stage.get("name")
        _require(isinstance(name, str) and name, "stage name required")
        _require(name not in names, "stage names must be unique")
        names.add(name)
        _require(stage.get("mode") in STAGE_MODES, f"stage {name} has invalid mode")
        _require(int(stage.get("samples", 0)) > 0, f"stage {name} requires samples")
        latency = stage.get("latency_ms")
        _require(isinstance(latency, dict), f"stage {name} latency required")
        p50 = float(latency.get("p50", -1.0))
        p95 = float(latency.get("p95", -1.0))
        p99 = float(latency.get("p99", -1.0))
        maximum = float(latency.get("maximum", -1.0))
        _require(0.0 <= p50 <= p95 <= p99 <= maximum, f"stage {name} percentiles are not monotonic")
        _require(int(stage.get("physical_read_bytes", -1)) >= 0, f"stage {name} read bytes invalid")
        peak_pss = stage.get("peak_pss_bytes")
        _require(peak_pss is None or int(peak_pss) >= 0, f"stage {name} PSS invalid")

    resources = report.get("resources")
    _require(isinstance(resources, dict) and resources, "resource ledger required")
    for name, resource in resources.items():
        hard_limit = int(resource.get("hard_limit_bytes", -1))
        current = int(resource.get("current_bytes", -1))
        peak = int(resource.get("peak_bytes", -1))
        _require(hard_limit >= 0, f"resource {name} hard limit invalid")
        _require(0 <= current <= hard_limit, f"resource {name} current bytes exceed hard limit")
        _require(0 <= peak <= hard_limit, f"resource {name} peak bytes exceed hard limit")
        _require(current <= peak, f"resource {name} current bytes exceed peak")
        for counter in ("cache_hits", "cache_misses", "evictions"):
            _require(int(resource.get(counter, -1)) >= 0, f"resource {name} {counter} invalid")

    correctness = report.get("correctness")
    _require(isinstance(correctness, dict) and correctness, "correctness gates required")
    _require(all(value is True for value in correctness.values()), "one or more correctness gates failed")

    gates = report.get("gates")
    _require(isinstance(gates, dict), "performance gates required")
    random_stage = next(stage for stage in stages if stage["name"] == "random_hot_scroll")
    adjacent_stage = next(stage for stage in stages if stage["name"] == "adjacent_hot_scroll")
    _require(
        float(random_stage["latency_ms"]["p95"]) <= float(gates["random_p95_ms_max"]),
        "random P95 gate failed",
    )
    _require(
        float(adjacent_stage["latency_ms"]["p95"]) <= float(gates["adjacent_p95_ms_max"]),
        "adjacent P95 gate failed",
    )
    _require(int(gates["maximum_source_read_bytes"]) <= 65_536, "source-read gate weakened")
    _require(float(gates["checkpoint_overhead_ratio_max"]) <= 0.002, "checkpoint gate weakened")
    _require(int(gates["payload_data_loss_bytes_max"]) == 0, "data-loss gate weakened")


def main() -> int:
    parser = argparse.ArgumentParser(description="Build and validate the unified ZENITH benchmark report")
    parser.add_argument("--massivedoc", type=Path, required=True)
    parser.add_argument("--global-layout", type=Path, required=True)
    parser.add_argument("--hot-scroll", type=Path, required=True)
    parser.add_argument("--commit-sha", default=os.environ.get("GITHUB_SHA", "unknown"))
    parser.add_argument("--compiler", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--build-type", default="Release")
    parser.add_argument("--runner-os", default=os.environ.get("RUNNER_OS", sys.platform))
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    report = build_giant_record_report(
        massivedoc=json.loads(args.massivedoc.read_text(encoding="utf-8")),
        global_layout=json.loads(args.global_layout.read_text(encoding="utf-8")),
        hot_scroll=json.loads(args.hot_scroll.read_text(encoding="utf-8")),
        commit_sha=args.commit_sha,
        compiler=args.compiler,
        build_type=args.build_type,
        runner_os=args.runner_os,
    )
    text = json.dumps(report, indent=2, sort_keys=True) + "\n"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(text, encoding="utf-8")
    print(text, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
