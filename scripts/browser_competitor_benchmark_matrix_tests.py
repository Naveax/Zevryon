#!/usr/bin/env python3
from __future__ import annotations

from browser_competitor_benchmark_executor import RawCaseResult
from browser_competitor_benchmark_matrix import BenchmarkTimeouts, execute_benchmark_matrix
from browser_competitor_benchmark_plan import plan_benchmark_cases
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


def evidence_context(plan) -> EvidenceContext:
    suffix = "1" if plan.mode == "virtualized" else "2"
    return EvidenceContext(
        host_platform="Linux",
        host_arch="x86_64",
        system_fingerprint="a" * 64,
        harness_schema="zevryon.competitor.giant-document.v2",
        corpus_sha256="b" * 64,
        scenario_fingerprint=suffix * 64,
    )


def main() -> int:
    plans = plan_benchmark_cases(
        ["chrome", "servo"],
        ["virtualized", "native-dom"],
    )
    context_calls: list[tuple[str, str]] = []
    runner_calls: list[tuple[str, str, int]] = []

    def context_for(plan):
        context_calls.append((plan.competitor, plan.mode))
        return evidence_context(plan)

    def run_case(plan, timeout_seconds: int) -> RawCaseResult:
        runner_calls.append((plan.competitor, plan.mode, timeout_seconds))
        if plan.mode == "virtualized":
            return RawCaseResult(
                status="success",
                runtime_identity=(
                    "Google Chrome; adapter=playwright; browser_type=chromium; "
                    "channel=chrome; distribution=branded-channel; version=123-test"
                ),
                measurements={"query_milliseconds_p95": 1.0},
            )
        return RawCaseResult(
            status="error",
            reason="RuntimeError: native test failure",
            measurements={"setup_started": True},
        )

    records = execute_benchmark_matrix(
        plans,
        payload_bytes=8192,
        timeouts=BenchmarkTimeouts(virtualized_seconds=180, native_dom_seconds=420),
        context_for=context_for,
        run_case=run_case,
    )
    require(len(records) == 4, "matrix output count changed")
    require(
        [(record["competitor"], record["mode"]) for record in records]
        == [
            ("chrome", "virtualized"),
            ("chrome", "native-dom"),
            ("servo", "virtualized"),
            ("servo", "native-dom"),
        ],
        "matrix output order changed",
    )
    require(
        context_calls == [("chrome", "virtualized"), ("chrome", "native-dom")],
        "context factory was called for unwired cases",
    )
    require(
        runner_calls
        == [
            ("chrome", "virtualized", 180),
            ("chrome", "native-dom", 420),
        ],
        "case runner call set or timeout mapping changed",
    )

    for record in records:
        validate_terminal_record(record)
    require(records[0]["status"] == "success", "virtualized success was lost")
    require(records[0]["timeout_seconds"] == 180, "virtualized timeout receipt was lost")
    require(records[1]["status"] == "error", "native failure was lost")
    require(records[1]["timeout_seconds"] == 420, "native timeout receipt was lost")
    require(records[2]["status"] == "unsupported", "Servo virtualized case was not unsupported")
    require(records[3]["status"] == "unsupported", "Servo native case was not unsupported")
    require(
        "timeout_seconds" not in records[2] and "timeout_seconds" not in records[3],
        "unwired case fabricated execution timeout evidence",
    )

    chrome_virtual = plan_benchmark_cases(["chrome"], ["virtualized"])
    context_failure = execute_benchmark_matrix(
        chrome_virtual,
        payload_bytes=8192,
        timeouts=BenchmarkTimeouts(180, 420),
        context_for=lambda plan: (_ for _ in ()).throw(ValueError("bad metadata")),
        run_case=lambda plan, timeout: (_ for _ in ()).throw(
            AssertionError("runner must not execute after context failure")
        ),
    )
    require(len(context_failure) == 1, "context failure lost exact-one terminal record")
    require(context_failure[0]["status"] == "invalid", "context failure was not invalid")
    require(
        "bad metadata" in str(context_failure[0]["reason"]),
        "context failure reason was lost",
    )
    require(context_failure[0]["timeout_seconds"] == 180, "context failure lost timeout receipt")
    validate_terminal_record(context_failure[0])

    runner_failure = execute_benchmark_matrix(
        chrome_virtual,
        payload_bytes=8192,
        timeouts=BenchmarkTimeouts(180, 420),
        context_for=evidence_context,
        run_case=lambda plan, timeout: (_ for _ in ()).throw(RuntimeError("boom")),
    )
    require(len(runner_failure) == 1, "runner exception lost exact-one terminal record")
    require(runner_failure[0]["status"] == "error", "runner exception was not error")
    require("boom" in str(runner_failure[0]["reason"]), "runner exception reason was lost")
    validate_terminal_record(runner_failure[0])

    malformed = execute_benchmark_matrix(
        chrome_virtual,
        payload_bytes=8192,
        timeouts=BenchmarkTimeouts(180, 420),
        context_for=evidence_context,
        run_case=lambda plan, timeout: RawCaseResult(
            status="success",
            runtime_identity="Chrome 123",
            measurements={"bad": float("nan")},
        ),
    )
    require(len(malformed) == 1, "malformed raw result lost terminal record")
    require(malformed[0]["status"] == "invalid", "malformed raw result was not invalid")
    require("NaN" in str(malformed[0]["reason"]), "malformed raw reason was lost")
    validate_terminal_record(malformed[0])

    wrong_type = execute_benchmark_matrix(
        chrome_virtual,
        payload_bytes=8192,
        timeouts=BenchmarkTimeouts(180, 420),
        context_for=evidence_context,
        run_case=lambda plan, timeout: {"status": "success"},  # type: ignore[return-value]
    )
    require(wrong_type[0]["status"] == "invalid", "wrong runner result type was not invalid")
    require(
        "RawCaseResult" in str(wrong_type[0]["reason"]),
        "wrong result type reason was lost",
    )

    duplicate_plans = [plans[0], plans[0]]
    require_value_error(
        lambda: execute_benchmark_matrix(
            duplicate_plans,
            payload_bytes=8192,
            timeouts=BenchmarkTimeouts(180, 420),
            context_for=context_for,
            run_case=run_case,
        ),
        "duplicate competitor/mode case entered matrix",
    )
    require_value_error(
        lambda: execute_benchmark_matrix(
            [],
            payload_bytes=8192,
            timeouts=BenchmarkTimeouts(180, 420),
            context_for=context_for,
            run_case=run_case,
        ),
        "empty benchmark matrix was accepted",
    )
    require_value_error(
        lambda: execute_benchmark_matrix(
            plans,
            payload_bytes=0,
            timeouts=BenchmarkTimeouts(180, 420),
            context_for=context_for,
            run_case=run_case,
        ),
        "non-positive matrix payload was accepted",
    )
    require_value_error(
        lambda: BenchmarkTimeouts(0, 420).validate(),
        "zero virtualized timeout was accepted",
    )
    require_value_error(
        lambda: BenchmarkTimeouts(180, -1).validate(),
        "negative native timeout was accepted",
    )

    print("Zevryon competitor benchmark matrix tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
