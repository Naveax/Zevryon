#!/usr/bin/env python3
from __future__ import annotations

from browser_competitor_benchmark_plan import (
    BENCHMARK_MODES,
    DEFAULT_COMPETITORS,
    EXECUTABLE_ADAPTERS,
    BenchmarkCasePlan,
    plan_benchmark_cases,
    unsupported_case_record,
)
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


def main() -> int:
    require(DEFAULT_COMPETITORS == ("chromium", "firefox"), "default competitor drift")
    require(BENCHMARK_MODES == ("virtualized", "native-dom"), "mode order drift")
    require(EXECUTABLE_ADAPTERS == {"playwright", "webdriver"}, "executable adapter set drift")

    default_cases = plan_benchmark_cases()
    require(len(default_cases) == 4, "default matrix must remain 2 competitors x 2 modes")
    require(
        [(case.competitor, case.mode) for case in default_cases]
        == [
            ("chromium", "virtualized"),
            ("chromium", "native-dom"),
            ("firefox", "virtualized"),
            ("firefox", "native-dom"),
        ],
        "default benchmark order changed",
    )
    require(all(case.executable for case in default_cases), "default cases stopped being executable")

    playwright = plan_benchmark_cases(["chrome", "edge", "webkit"], ["virtualized"])
    require(
        [case.competitor for case in playwright] == ["chrome", "edge", "webkit"],
        "requested competitor order changed",
    )
    require(all(case.adapter == "playwright" for case in playwright), "Playwright routing mismatch")
    require(all(case.executable for case in playwright), "Playwright cases were not executable")

    webdriver = plan_benchmark_cases(["servo", "ladybird"], ["virtualized"])
    require(len(webdriver) == 2, "WebDriver adapter plan count mismatch")
    require(all(case.adapter == "webdriver" for case in webdriver), "WebDriver routing mismatch")
    require(all(case.executable for case in webdriver), "WebDriver cases were not executable")
    require(all(case.reason is None for case in webdriver), "executable WebDriver case carried unsupported reason")

    full = plan_benchmark_cases(
        ["chrome", "firefox", "edge", "webkit", "servo", "ladybird"],
        ["virtualized", "native-dom"],
    )
    require(len(full) == 12, "canonical full-set matrix must contain 12 cases")
    require(all(case.executable for case in full), "canonical adapter matrix is not fully dispatchable")

    forced_pending = BenchmarkCasePlan(
        competitor="servo",
        canonical_name="Servo",
        canonical=True,
        adapter="webdriver",
        mode="virtualized",
        executable=False,
        reason="synthetic adapter capability gap",
    )
    unsupported = unsupported_case_record(forced_pending, payload_bytes=64 * 1024 * 1024)
    validate_terminal_record(unsupported)
    require(unsupported["status"] == "unsupported", "forced unsupported state mismatch")
    require(unsupported["competitor"] == "servo", "forced unsupported identity lost")

    tampered = BenchmarkCasePlan(
        competitor="servo",
        canonical_name="Not Servo",
        canonical=True,
        adapter="webdriver",
        mode="virtualized",
        executable=False,
        reason="synthetic adapter capability gap",
    )
    require_value_error(
        lambda: unsupported_case_record(tampered, payload_bytes=1),
        "planner identity drift bypassed canonical registry",
    )
    require_value_error(
        lambda: plan_benchmark_cases(["chrome", "chrome"]),
        "duplicate competitor request was accepted",
    )
    require_value_error(
        lambda: plan_benchmark_cases(["not-a-browser"]),
        "unknown competitor request was accepted",
    )
    require_value_error(
        lambda: plan_benchmark_cases(["chrome"], []),
        "empty mode request was accepted",
    )
    require_value_error(
        lambda: plan_benchmark_cases(["chrome"], ["virtualized", "virtualized"]),
        "duplicate benchmark mode was accepted",
    )
    require_value_error(
        lambda: plan_benchmark_cases(["chrome"], ["not-a-mode"]),
        "unknown benchmark mode was accepted",
    )
    require_value_error(
        lambda: unsupported_case_record(playwright[0], payload_bytes=1),
        "executable case was serialized as unsupported",
    )
    require_value_error(
        lambda: unsupported_case_record(forced_pending, payload_bytes=0),
        "non-positive payload was accepted",
    )

    print("Zevryon competitor benchmark planner tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
