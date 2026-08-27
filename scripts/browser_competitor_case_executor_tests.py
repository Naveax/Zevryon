#!/usr/bin/env python3
from __future__ import annotations

from browser_competitor_case_executor import (
    _query_summary,
    execute_case,
    failure_case_record,
    percentile,
    process_scope_metrics,
    webdriver_failure_status,
    worker_entry,
)
from browser_competitor_process_scope import (
    ProcessIdentity,
    ProcessScopeSnapshot,
    ProcessScopeUnavailable,
)
from browser_competitor_scenario_contract import ScenarioContractInvalid
from browser_competitor_webdriver import WebDriverProtocolError, WebDriverTransportError
from browser_competitor_webdriver_runtime import (
    WebDriverRuntimeInvalid,
    WebDriverRuntimeLaunchError,
    WebDriverRuntimeUnavailable,
)
from browser_competitor_webdriver_scenario import WebDriverScenarioInvalid


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def require_raises(exc_type, callable_, message: str) -> None:
    try:
        callable_()
    except exc_type:
        return
    raise AssertionError(message)


class FakeMonitor:
    def __init__(self, *, valid: bool = True) -> None:
        self.valid = valid
        self.peak_bytes = 9_000_000
        self.observed_receipts = [
            {"pid": 101, "create_time_ns": 1_000},
            {"pid": 102, "create_time_ns": 2_000},
        ]


class FakeQueue:
    def __init__(self) -> None:
        self.values: list[object] = []

    def put(self, value: object) -> None:
        self.values.append(value)


def main() -> int:
    require(percentile([1.0, 2.0, 3.0], 50.0) == 2.0, "median drifted")
    require(percentile([1.0, 3.0], 50.0) == 2.0, "interpolated percentile drifted")
    require_raises(ValueError, lambda: percentile([], 50.0), "empty percentile accepted")

    summary = _query_summary([1.0, 2.0, 3.0, 4.0])
    require(summary["query_milliseconds_p50"] == 2.5, "p50 summary drifted")
    require(summary["query_milliseconds_max"] == 4.0, "max summary drifted")
    require(summary["query_milliseconds_mean"] == 2.5, "mean summary drifted")

    non_success = failure_case_record(
        "servo",
        "virtualized",
        4096,
        "unavailable",
        "missing test binary",
    )
    require(non_success["competitor"] == "servo", "failure competitor identity lost")
    require(non_success["canonical_name"] == "Servo", "failure canonical identity lost")
    require(non_success["adapter"] == "webdriver", "failure adapter identity lost")
    require(non_success["status"] == "unavailable", "failure status drifted")

    mappings = [
        (WebDriverRuntimeUnavailable("missing"), "unavailable"),
        (WebDriverRuntimeInvalid("bad identity"), "invalid"),
        (WebDriverScenarioInvalid("bad scenario"), "invalid"),
        (ScenarioContractInvalid("bad contract"), "invalid"),
        (ProcessScopeUnavailable("no psutil"), "invalid"),
        (WebDriverProtocolError("unknown command: execute/async"), "unsupported"),
        (WebDriverProtocolError("invalid argument: bad"), "error"),
        (WebDriverRuntimeLaunchError("early exit"), "error"),
        (WebDriverTransportError("connection reset"), "error"),
    ]
    for exc, expected in mappings:
        status, reason = webdriver_failure_status(exc)
        require(status == expected, f"failure mapping drifted for {type(exc).__name__}")
        require(type(exc).__name__ in reason, "failure reason lost exception identity")

    setup = ProcessScopeSnapshot(
        identities=(ProcessIdentity(pid=101, create_time_ns=1_000),),
        resident_bytes=7_000_000,
    )
    queries = ProcessScopeSnapshot(
        identities=(
            ProcessIdentity(pid=101, create_time_ns=1_000),
            ProcessIdentity(pid=102, create_time_ns=2_000),
        ),
        resident_bytes=8_000_000,
    )
    monitor = FakeMonitor()
    memory = process_scope_metrics(monitor, setup, queries)
    require(memory["memory_metric_status"] == "valid", "valid process scope rejected")
    require(memory["browser_scope_resident_mb_after_setup"] == 7.0, "setup memory drifted")
    require(memory["browser_scope_resident_mb_after_queries"] == 8.0, "query memory drifted")
    require(memory["browser_scope_peak_mb"] == 9.0, "peak memory drifted")
    require(
        memory["browser_scope_observed_receipts"] == monitor.observed_receipts,
        "observed receipts lost",
    )

    empty = ProcessScopeSnapshot(identities=(), resident_bytes=0)
    invalid_memory = process_scope_metrics(FakeMonitor(valid=False), empty, queries)
    require(invalid_memory["memory_metric_status"] == "invalid", "empty process scope accepted")
    require(
        "no browser process identity was observed" in str(invalid_memory["memory_metric_reason"]),
        "invalid process-scope reason lost",
    )

    import browser_competitor_case_executor as module

    original_playwright = module._playwright_case
    original_webdriver = module._webdriver_case
    original_execute = module.execute_case
    try:
        module._playwright_case = lambda *args: {"route": "playwright"}
        module._webdriver_case = lambda *args: {"route": "webdriver"}

        routed_playwright = execute_case(
            adapter="playwright",
            competitor="chrome",
            mode="virtualized",
            payload_bytes=1,
            query_count=1,
            slice_bytes=1,
            case_timeout_seconds=1,
            evidence_kwargs={},
        )
        require(routed_playwright == {"route": "playwright"}, "Playwright executor routing drifted")

        routed_webdriver = execute_case(
            adapter="webdriver",
            competitor="servo",
            mode="native-dom",
            payload_bytes=1,
            query_count=1,
            slice_bytes=1,
            case_timeout_seconds=1,
            evidence_kwargs={},
        )
        require(routed_webdriver == {"route": "webdriver"}, "WebDriver executor routing drifted")

        unsupported = execute_case(
            adapter="future-adapter",
            competitor="servo",
            mode="virtualized",
            payload_bytes=1,
            query_count=1,
            slice_bytes=1,
            case_timeout_seconds=1,
            evidence_kwargs={},
        )
        require(unsupported["status"] == "unsupported", "unknown adapter was not fail-closed")

        queue = FakeQueue()
        module.execute_case = lambda **_kwargs: (_ for _ in ()).throw(RuntimeError("worker boom"))
        worker_entry(
            "webdriver",
            "servo",
            "virtualized",
            1,
            1,
            1,
            1,
            {},
            queue,
        )
        require(len(queue.values) == 1, "worker did not emit exactly one result")
        result = queue.values[0]
        require(isinstance(result, dict), "worker result is not a mapping")
        require(result["status"] == "error", "worker exception was not serialized")
        require("worker boom" in str(result["reason"]), "worker exception reason lost")
    finally:
        module._playwright_case = original_playwright
        module._webdriver_case = original_webdriver
        module.execute_case = original_execute

    print("Zevryon competitor case executor tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
