#!/usr/bin/env python3
from __future__ import annotations

from browser_competitor_servo import (
    SERVO_BINARY_ENV,
    ServoAdapterInvalid,
    ServoAdapterUnavailable,
    build_servo_launch_plan,
    parse_servo_version,
    resolve_servo_binary,
    runtime_identity,
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def require_raises(exc_type, callable_, message: str) -> None:
    try:
        callable_()
    except exc_type:
        return
    raise AssertionError(message)


def main() -> int:
    require(
        resolve_servo_binary("/opt/servo/bin/servo", environment={}, which=lambda _name: None)
        == "/opt/servo/bin/servo",
        "explicit Servo path lost",
    )
    require(
        resolve_servo_binary(
            None,
            environment={SERVO_BINARY_ENV: "/env/servo"},
            which=lambda _name: None,
        )
        == "/env/servo",
        "Servo environment path lost",
    )

    calls: list[str] = []
    def fake_which(name: str) -> str | None:
        calls.append(name)
        return "/usr/bin/servoshell" if name == "servoshell" else None

    require(
        resolve_servo_binary(None, environment={}, which=fake_which)
        == "/usr/bin/servoshell",
        "PATH servoshell fallback failed",
    )
    require(calls == ["servo", "servoshell"], "Servo PATH resolution order drifted")
    require_raises(
        ServoAdapterUnavailable,
        lambda: resolve_servo_binary(None, environment={}, which=lambda _name: None),
        "missing Servo binary was accepted",
    )

    plan = build_servo_launch_plan("/opt/servo", 47321)
    require(
        plan.command
        == ("/opt/servo", "--headless", "--webdriver=47321", "about:blank"),
        "Servo launch command drifted",
    )
    require(plan.webdriver_url == "http://127.0.0.1:47321", "WebDriver URL drifted")
    require(plan.identity_command == ("/opt/servo", "--version"), "identity command drifted")
    require(
        parse_servo_version("Servo 0.0.1\n") == "Servo 0.0.1",
        "Servo stdout version parsing failed",
    )
    require(
        parse_servo_version("", "Servo nightly abc123\n") == "Servo nightly abc123",
        "Servo stderr version parsing failed",
    )
    require(
        runtime_identity(plan, "Servo nightly abc123")
        == "servo|binary=/opt/servo|version=Servo nightly abc123|webdriver=127.0.0.1:47321|headless=true",
        "Servo runtime identity drifted",
    )

    for bad_port in (0, 65536, -1, True):
        require_raises(
            ServoAdapterInvalid,
            lambda bad_port=bad_port: build_servo_launch_plan("/opt/servo", bad_port),
            f"invalid Servo port accepted: {bad_port!r}",
        )
    require_raises(
        ServoAdapterInvalid,
        lambda: parse_servo_version("", ""),
        "empty Servo version was accepted",
    )

    print("Zevryon Servo adapter tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
