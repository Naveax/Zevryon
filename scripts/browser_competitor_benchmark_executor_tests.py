#!/usr/bin/env python3
from __future__ import annotations

import math

from browser_competitor_benchmark_executor import (
    RawCaseResult,
    terminalize_executable_case,
    terminalize_unwired_case,
)
from browser_competitor_benchmark_plan import BenchmarkCasePlan, plan_benchmark_cases
from browser_competitor_evidence_context import EvidenceContext
from browser_competitor_registry import validate_terminal_record


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def require_value_error(callable_, message: str) -> None:
    try:
        callable_()
    except ValueError:
        return
    raise AssertionError(message)


def context() -> EvidenceContext:
    return EvidenceContext(
        host_platform="Linux",
        host_arch="x86_64",
        system_fingerprint="1" * 64,
        harness_schema="zevryon.competitor.giant-document.v2",
        corpus_sha256="2" * 64,
        scenario_fingerprint="3" * 64,
    )


def main() -> int:
    chrome = plan_benchmark_cases(["chrome"], ["virtualized"])[0]
    success = RawCaseResult(
        status="success",
        runtime_identity=(
            "Google Chrome; adapter=playwright; browser_type=chromium; "
            "channel=chrome; distribution=branded-channel; version=123-test"
        ),
        measurements={"query_milliseconds_p95": 1.25, "peak_pss_mb": 42.0},
    )
    success_record = terminalize_executable_case(
        chrome,
        success,
        context(),
        payload_bytes=8192,
    )
    validate_terminal_record(success_record)
    require(success_record["status"] == "success", "success status was lost")
    require(success_record["competitor"] == "chrome", "success competitor identity was lost")
    require(success_record["mode"] == "virtualized", "benchmark mode was lost")
    require(success_record["payload_bytes"] == 8192, "payload size was lost")
    require(
        success_record["measurements"]["query_milliseconds_p95"] == 1.25,
        "success measurements were not preserved",
    )
    require(
        success_record["system_fingerprint"] == "1" * 64,
        "success evidence context was not applied",
    )

    unavailable = RawCaseResult(
        status="unavailable",
        reason="RuntimeError: Browser distribution 'chrome' is not found",
        measurements={"launch_attempted": True},
    )
    unavailable_record = terminalize_executable_case(
        chrome,
        unavailable,
        context(),
        payload_bytes=8192,
    )
    validate_terminal_record(unavailable_record)
    require(unavailable_record["status"] == "unavailable", "unavailable state was lost")
    require(
        "distribution" in str(unavailable_record["reason"]),
        "unavailable reason detail was lost",
    )
    require(
        unavailable_record["measurements"] == {"launch_attempted": True},
        "failure measurements were not preserved",
    )

    timeout = RawCaseResult(
        status="timeout",
        reason="case exceeded bounded timeout of 180 seconds",
    )
    timeout_record = terminalize_executable_case(
        chrome,
        timeout,
        context(),
        payload_bytes=8192,
    )
    validate_terminal_record(timeout_record)
    require(timeout_record["status"] == "timeout", "timeout state was lost")

    servo = plan_benchmark_cases(["servo"], ["native-dom"])[0]
    servo_record = terminalize_unwired_case(servo, payload_bytes=8192)
    validate_terminal_record(servo_record)
    require(servo_record["status"] == "unsupported", "Servo did not fail closed as unsupported")
    require(servo_record["adapter"] == "webdriver", "Servo adapter identity was lost")
    require(servo_record["mode"] == "native-dom", "Servo mode was lost")
    require(servo_record["measurements"] == {}, "unwired case fabricated measurements")

    require_value_error(
        lambda: terminalize_executable_case(
            servo,
            success,
            context(),
            payload_bytes=8192,
        ),
        "non-executable plan entered executable terminalizer",
    )
    require_value_error(
        lambda: terminalize_unwired_case(chrome, payload_bytes=8192),
        "executable plan entered unwired terminalizer",
    )
    require_value_error(
        lambda: RawCaseResult(status="success").validate(),
        "success without runtime identity was accepted",
    )
    require_value_error(
        lambda: RawCaseResult(
            status="success",
            runtime_identity="Chrome 123",
            reason="should not exist",
        ).validate(),
        "success with failure reason was accepted",
    )
    require_value_error(
        lambda: RawCaseResult(status="error").validate(),
        "failure without reason was accepted",
    )
    require_value_error(
        lambda: RawCaseResult(status="not-a-state", reason="bad").validate(),
        "unknown terminal state was accepted",
    )
    require_value_error(
        lambda: RawCaseResult(
            status="success",
            runtime_identity="Chrome 123",
            measurements={"bad": math.nan},
        ).validate(),
        "non-canonical NaN measurement entered evidence",
    )
    require_value_error(
        lambda: terminalize_executable_case(
            chrome,
            success,
            context(),
            payload_bytes=0,
        ),
        "non-positive payload entered terminal evidence",
    )

    tampered = BenchmarkCasePlan(
        competitor="chrome",
        canonical_name="Not Chrome",
        canonical=True,
        adapter="playwright",
        mode="virtualized",
        executable=True,
    )
    require_value_error(
        lambda: terminalize_executable_case(
            tampered,
            success,
            context(),
            payload_bytes=8192,
        ),
        "tampered planner identity bypassed registry authority",
    )

    print("Zevryon competitor benchmark executor tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
