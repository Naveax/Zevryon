#!/usr/bin/env python3
from __future__ import annotations

from browser_competitor_benchmark_plan import (
    BENCHMARK_MODES,
    DEFAULT_COMPETITORS,
    plan_benchmark_cases,
    unsupported_case_record,
)


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
    default_cases = plan_benchmark_cases()
    require(DEFAULT_COMPETITORS == ("chromium", "firefox"), "default competitor drift")
    require(BENCHMARK_MODES == ("virtualized", "native-dom"), "mode order drift")
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

    branded = plan_benchmark_cases(["chrome", "edge", "webkit"], ["virtualized"])
    require(
        [case.competitor for case in branded] == ["chrome", "edge", "webkit"],
        "requested competitor order changed",
    )
    require(all(case.adapter == "playwright" for case in branded), "Playwright adapter mismatch")
    require(all(case.executable for case in branded), "Playwright cases were not executable")

    pending = plan_benchmark_cases(["servo", "ladybird"], ["virtualized"])
    require(len(pending) == 2, "pending adapter plan count mismatch")
    require(not pending[0].executable and pending[0].adapter == "webdriver", "Servo routing mismatch")
    require(
        not pending[1].executable and pending[1].adapter == "ladybird-headless",
        "Ladybird routing mismatch",
    )

    servo_record = unsupported_case_record(pending[0], payload_bytes=64 * 1024 * 1024)
    require(servo_record["status"] == "unsupported", "Servo unsupported state mismatch")
    require(servo_record["competitor"] == "servo", "Servo identity lost")
    require(servo_record["canonical"] is True, "Servo canonical flag lost")
    require(servo_record["adapter"] == "webdriver", "Servo adapter identity lost")
    require(isinstance(servo_record["reason"], str) and servo_record["reason"], "Servo reason missing")

    full = plan_benchmark_cases(
        ["chrome", "firefox", "edge", "webkit", "servo", "ladybird"],
        ["virtualized", "native-dom"],
    )
    require(len(full) == 12, "canonical full-set matrix must contain 12 cases")
    require(
        sum(1 for case in full if case.executable) == 8,
        "current Playwright-executable canonical case count mismatch",
    )
    require(
        sum(1 for case in full if not case.executable) == 4,
        "pending Servo/Ladybird case count mismatch",
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
        lambda: unsupported_case_record(branded[0], payload_bytes=1),
        "executable case was serialized as unsupported",
    )
    require_value_error(
        lambda: unsupported_case_record(pending[0], payload_bytes=0),
        "non-positive payload was accepted",
    )

    print("Zevryon competitor benchmark planner tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
