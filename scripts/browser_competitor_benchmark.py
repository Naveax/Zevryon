#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import multiprocessing as mp
from pathlib import Path
import queue
from typing import Any, Mapping

from browser_competitor_benchmark_evidence import (
    HARNESS_SCHEMA,
    evidence_identity,
    host_metadata,
    scenario_fingerprint,
    synthetic_corpus_sha256,
)
from browser_competitor_benchmark_plan import (
    BenchmarkCasePlan,
    plan_benchmark_cases,
    unsupported_case_record,
)
from browser_competitor_case_executor import failure_case_record, worker_entry
from browser_competitor_registry import leadership_coverage, validate_terminal_record


def _validate_case_result(
    plan: BenchmarkCasePlan,
    payload_bytes: int,
    result: Mapping[str, Any],
) -> dict[str, Any]:
    try:
        validate_terminal_record(result)
    except ValueError as exc:
        return dict(
            failure_case_record(
                plan.competitor,
                plan.mode,
                payload_bytes,
                "invalid",
                f"worker terminal evidence validation failed: {exc}",
                worker_result=dict(result),
            )
        )

    mismatches: list[str] = []
    if result.get("competitor") != plan.competitor:
        mismatches.append("competitor")
    if result.get("browser") != plan.competitor:
        mismatches.append("browser")
    if result.get("mode") != plan.mode:
        mismatches.append("mode")
    if result.get("payload_bytes") != payload_bytes:
        mismatches.append("payload_bytes")
    if result.get("adapter") != plan.adapter:
        mismatches.append("adapter")
    if mismatches:
        return dict(
            failure_case_record(
                plan.competitor,
                plan.mode,
                payload_bytes,
                "invalid",
                "worker case identity drifted: " + ", ".join(mismatches),
                worker_result=dict(result),
            )
        )

    if result.get("status") == "success":
        if result.get("memory_metric_status") != "valid":
            return dict(
                failure_case_record(
                    plan.competitor,
                    plan.mode,
                    payload_bytes,
                    "invalid",
                    "successful worker result lacks valid browser process-scope memory evidence",
                    worker_result=dict(result),
                )
            )
        query_details = result.get("query_details")
        query_count = result.get("query_count")
        if (
            not isinstance(query_count, int)
            or isinstance(query_count, bool)
            or query_count <= 0
            or not isinstance(query_details, list)
            or len(query_details) != query_count
        ):
            return dict(
                failure_case_record(
                    plan.competitor,
                    plan.mode,
                    payload_bytes,
                    "invalid",
                    "successful worker result has inconsistent query evidence",
                    worker_result=dict(result),
                )
            )

    return dict(result)


def run_case(
    plan: BenchmarkCasePlan,
    payload_bytes: int,
    query_count: int,
    slice_bytes: int,
    timeout_seconds: int,
    evidence_kwargs: dict[str, str],
) -> dict[str, Any]:
    if not plan.executable:
        return dict(unsupported_case_record(plan, payload_bytes=payload_bytes))

    context = mp.get_context("spawn")
    output_queue = context.Queue()
    process = context.Process(
        target=worker_entry,
        args=(
            plan.adapter,
            plan.competitor,
            plan.mode,
            payload_bytes,
            query_count,
            slice_bytes,
            timeout_seconds,
            evidence_kwargs,
            output_queue,
        ),
    )
    process.start()
    process.join(timeout_seconds)
    if process.is_alive():
        process.terminate()
        process.join(15)
        return dict(
            failure_case_record(
                plan.competitor,
                plan.mode,
                payload_bytes,
                "timeout",
                f"case exceeded declared timeout of {timeout_seconds} seconds",
                timeout_seconds=timeout_seconds,
            )
        )

    try:
        raw_result = output_queue.get(timeout=5)
    except queue.Empty:
        return dict(
            failure_case_record(
                plan.competitor,
                plan.mode,
                payload_bytes,
                "error",
                f"worker exited with code {process.exitcode} without a result",
                worker_exit_code=process.exitcode,
            )
        )

    if not isinstance(raw_result, dict):
        return dict(
            failure_case_record(
                plan.competitor,
                plan.mode,
                payload_bytes,
                "invalid",
                "worker result is not a JSON object",
                worker_result_type=type(raw_result).__name__,
            )
        )
    return _validate_case_result(plan, payload_bytes, raw_result)


def zevryon_summary(report: dict[str, Any]) -> dict[str, Any]:
    layout = report["layout_window"]
    comparison = layout["comparison"]
    baseline = layout["baseline_layout"]
    return {
        "payload_bytes": int(report["giant_record_bytes"]),
        "layout_model": "deterministic average-advance fragments, not browser font shaping",
        "baseline_full_scan_seconds": float(comparison["baseline_seconds"]),
        "baseline_full_scan_peak_pss_mb": baseline.get("peak_pss_mb"),
        "baseline_source_bytes_read": int(comparison["baseline_source_bytes_read"]),
        "checkpoint_query_count": int(comparison["checkpoint_query_count"]),
        "checkpoint_seconds_p50": float(comparison["checkpoint_window_seconds_p50"]),
        "checkpoint_seconds_p95": float(comparison["checkpoint_window_seconds_p95"]),
        "checkpoint_seconds_p99": float(comparison["checkpoint_window_seconds_p99"]),
        "checkpoint_seconds_max": float(comparison["checkpoint_window_seconds_max"]),
        "checkpoint_peak_pss_mb_max": comparison.get("checkpoint_peak_pss_mb_max"),
        "checkpoint_source_bytes_read_max": int(comparison["checkpoint_source_bytes_read_max"]),
        "checkpoint_physical_bytes": int(comparison["checkpoint_physical_bytes"]),
        "checkpoint_overhead_ratio": float(comparison["checkpoint_overhead_ratio"]),
        "speedup_x_p50": float(comparison["speedup_x_p50"]),
        "speedup_x_p95": float(comparison["speedup_x_p95"]),
        "source_read_reduction_x_worst": float(comparison["source_read_reduction_x_worst"]),
        "checkpoint_build_seconds": float(comparison["checkpoint_build_seconds"]),
        "zero_payload_data_loss": bool(layout["zero_payload_data_loss"]),
    }


def _timeout_for_mode(
    mode: str,
    *,
    virtual_timeout_seconds: int,
    native_timeout_seconds: int,
) -> int:
    if mode == "virtualized":
        return virtual_timeout_seconds
    if mode == "native-dom":
        return native_timeout_seconds
    raise ValueError(f"unknown benchmark mode: {mode}")


def _coverage_by_mode(cases: list[dict[str, Any]]) -> dict[str, object]:
    output: dict[str, object] = {}
    for mode in ("virtualized", "native-dom"):
        canonical_records = [
            case
            for case in cases
            if case.get("mode") == mode and case.get("canonical") is True
        ]
        output[mode] = leadership_coverage(canonical_records)
    return output


def _competitor_sets(
    requested_competitors: list[str],
    cases: list[dict[str, Any]],
) -> dict[str, list[str]]:
    by_competitor = {
        competitor: [case for case in cases if case.get("competitor") == competitor]
        for competitor in requested_competitors
    }
    unavailable = [
        competitor
        for competitor, records in by_competitor.items()
        if records and all(record.get("status") == "unavailable" for record in records)
    ]
    available = [
        competitor
        for competitor, records in by_competitor.items()
        if records and any(record.get("status") != "unavailable" for record in records)
    ]
    successful = [
        competitor
        for competitor, records in by_competitor.items()
        if any(record.get("status") == "success" for record in records)
    ]
    fully_successful = [
        competitor
        for competitor, records in by_competitor.items()
        if records and all(record.get("status") == "success" for record in records)
    ]
    runtime_unsupported = [
        competitor
        for competitor, records in by_competitor.items()
        if any(record.get("status") == "unsupported" for record in records)
    ]
    return {
        "available_competitors": available,
        "unavailable_competitors": unavailable,
        "successfully_measured_competitors": successful,
        "fully_measured_competitors": fully_successful,
        "runtime_unsupported_competitors": runtime_unsupported,
    }


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Compare Zevryon giant-record access with explicitly identified "
            "browser/engine competitors"
        )
    )
    parser.add_argument("--zevryon-report", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--competitor",
        dest="competitors",
        action="append",
        help=(
            "competitor registry key; repeat to request multiple engines. "
            "Default remains auxiliary chromium plus firefox"
        ),
    )
    parser.add_argument("--payload-bytes", type=int, default=64 * 1024 * 1024)
    parser.add_argument("--query-count", type=int, default=21)
    parser.add_argument("--virtual-slice-bytes", type=int, default=128 * 1024)
    parser.add_argument("--virtual-timeout-seconds", type=int, default=180)
    parser.add_argument("--native-timeout-seconds", type=int, default=420)
    args = parser.parse_args()

    if (
        args.payload_bytes <= 0
        or args.query_count <= 0
        or args.virtual_slice_bytes <= 0
        or args.virtual_timeout_seconds <= 0
        or args.native_timeout_seconds <= 0
    ):
        parser.error("benchmark arguments must be positive")

    try:
        plans = plan_benchmark_cases(args.competitors)
    except ValueError as exc:
        parser.error(str(exc))

    zevryon_report = json.loads(args.zevryon_report.read_text(encoding="utf-8"))
    host = host_metadata()
    corpus_sha256 = synthetic_corpus_sha256(args.payload_bytes)

    cases: list[dict[str, Any]] = []
    for plan in plans:
        timeout_seconds = _timeout_for_mode(
            plan.mode,
            virtual_timeout_seconds=args.virtual_timeout_seconds,
            native_timeout_seconds=args.native_timeout_seconds,
        )
        scenario_sha256 = scenario_fingerprint(
            mode=plan.mode,
            payload_bytes=args.payload_bytes,
            query_count=args.query_count,
            virtual_slice_bytes=args.virtual_slice_bytes,
            timeout_seconds=timeout_seconds,
        )
        identity = evidence_identity(
            host=host,
            corpus_sha256=corpus_sha256,
            scenario_sha256=scenario_sha256,
        )
        cases.append(
            run_case(
                plan,
                args.payload_bytes,
                args.query_count,
                args.virtual_slice_bytes,
                timeout_seconds,
                identity.as_terminal_kwargs(),
            )
        )

    requested_competitors = list(dict.fromkeys(plan.competitor for plan in plans))
    executable_competitors = list(
        dict.fromkeys(plan.competitor for plan in plans if plan.executable)
    )
    unsupported_competitors = list(
        dict.fromkeys(plan.competitor for plan in plans if not plan.executable)
    )
    status_sets = _competitor_sets(requested_competitors, cases)
    virtual_failures = [
        case
        for case in cases
        if case["mode"] == "virtualized" and case["status"] != "success"
    ]
    all_failures = [case for case in cases if case["status"] != "success"]

    report = {
        "schema": HARNESS_SCHEMA,
        "host": {
            **host,
            "system_fingerprint": evidence_identity(
                host=host,
                corpus_sha256=corpus_sha256,
                scenario_sha256=scenario_fingerprint(
                    mode="virtualized",
                    payload_bytes=args.payload_bytes,
                    query_count=args.query_count,
                    virtual_slice_bytes=args.virtual_slice_bytes,
                    timeout_seconds=args.virtual_timeout_seconds,
                ),
            ).system_fingerprint,
        },
        "corpus_sha256": corpus_sha256,
        "payload_bytes": args.payload_bytes,
        "query_count": args.query_count,
        "virtual_slice_bytes": args.virtual_slice_bytes,
        "requested_competitors": requested_competitors,
        "executable_competitors": executable_competitors,
        "unsupported_competitors": unsupported_competitors,
        **status_sets,
        "scope_notes": [
            "Zevryon checkpoint timings include CLI process start, store open, checkpoint open, bounded read, and JSON serialization.",
            "Browser query timings are steady-context page timings after adapter-specific launch and payload setup.",
            "Browser memory is scoped only to descendants created after each adapter control baseline, with PID-plus-create-time identity, Linux PSS, and RSS fallback.",
            "A missing or empty browser process scope invalidates successful memory evidence instead of falling back to Python/driver process memory.",
            "Playwright and W3C WebDriver cases share the admitted Unicode corpus, DOM geometry, exact 800x720 inner viewport, deterministic offsets, warmup policy, and double-requestAnimationFrame timing boundary.",
            "Native DOM uses real browser text layout; Zevryon currently uses deterministic average-advance fragments.",
            "Branded Chrome/Edge channels are never replaced by bundled Chromium when unavailable.",
        ],
        "zevryon": zevryon_summary(zevryon_report),
        "browser_cases": cases,
        "leadership_coverage_by_mode": _coverage_by_mode(cases),
        "all_virtualized_cases_succeeded": not virtual_failures,
        "all_requested_cases_succeeded": not all_failures,
        "leadership_metric_gate_evaluated": False,
        "leadership_eligible": False,
    }
    text = json.dumps(report, indent=2, sort_keys=True) + "\n"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(text, encoding="utf-8")
    print(text, end="")
    return 1 if virtual_failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
