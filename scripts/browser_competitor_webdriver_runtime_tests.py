#!/usr/bin/env python3
from __future__ import annotations

import subprocess

from browser_competitor_webdriver_runtime import (
    PreparedWebDriverRuntime,
    WebDriverRuntimeInvalid,
    WebDriverRuntimeLaunchError,
    WebDriverRuntimeUnavailable,
    allocate_loopback_port,
    launch_prepared_runtime,
    prepare_webdriver_runtime,
    wait_for_webdriver_ready,
)


class FakeProcess:
    def __init__(self, *, returncode=None, timeout_on_first_wait: bool = False) -> None:
        self.returncode = returncode
        self.timeout_on_first_wait = timeout_on_first_wait
        self.terminated = False
        self.killed = False
        self.wait_calls = 0

    def poll(self):
        return self.returncode

    def terminate(self) -> None:
        self.terminated = True

    def kill(self) -> None:
        self.killed = True
        self.returncode = -9

    def wait(self, timeout=None):
        self.wait_calls += 1
        if self.timeout_on_first_wait and self.wait_calls == 1:
            raise subprocess.TimeoutExpired("fake", timeout)
        if self.returncode is None:
            self.returncode = 0
        return self.returncode


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
    port = allocate_loopback_port()
    require(1 <= port <= 65535, "ephemeral loopback port out of range")

    servo = prepare_webdriver_runtime(
        "servo",
        port=47001,
        explicit_binary="/opt/servo/servoshell",
        environment={},
        which=lambda _name: None,
        identity_runner=lambda command: (
            0,
            "Servo nightly abc123\n",
            "",
        ),
    )
    require(servo.competitor == "servo", "Servo runtime competitor drifted")
    require(
        servo.command
        == ("/opt/servo/servoshell", "--headless", "--webdriver=47001", "about:blank"),
        "Servo runtime launch command drifted",
    )
    require("Servo nightly abc123" in servo.runtime_identity, "Servo identity lost version")
    require(servo.identity_source == "servo --version", "Servo identity source drifted")

    ladybird = prepare_webdriver_runtime(
        "ladybird",
        port=47002,
        explicit_binary="/opt/ladybird/WebDriver",
        environment={},
        which=lambda _name: None,
        ladybird_hasher=lambda _path: "a" * 64,
    )
    require(ladybird.competitor == "ladybird", "Ladybird runtime competitor drifted")
    require(
        ladybird.command
        == (
            "/opt/ladybird/WebDriver",
            "--headless",
            "-l",
            "127.0.0.1",
            "-p",
            "47002",
        ),
        "Ladybird runtime launch command drifted",
    )
    require("sha256=" + "a" * 64 in ladybird.runtime_identity, "Ladybird binary hash lost")

    require_raises(
        WebDriverRuntimeUnavailable,
        lambda: prepare_webdriver_runtime(
            "servo",
            port=47003,
            explicit_binary=None,
            environment={},
            which=lambda _name: None,
        ),
        "missing Servo runtime was not classified unavailable",
    )
    require_raises(
        WebDriverRuntimeInvalid,
        lambda: prepare_webdriver_runtime(
            "servo",
            port=47004,
            explicit_binary="/opt/servo/servoshell",
            environment={},
            which=lambda _name: None,
            identity_runner=lambda _command: (2, "", "bad identity"),
        ),
        "failing Servo identity command was admitted",
    )
    require_raises(
        ValueError,
        lambda: prepare_webdriver_runtime("not-an-engine", port=47005),
        "unknown WebDriver competitor was accepted",
    )

    ready_process = FakeProcess()
    ready = wait_for_webdriver_ready(
        ready_process,
        "http://127.0.0.1:47006",
        timeout_seconds=0.2,
        interval_seconds=0.001,
        status_reader=lambda _url: {"ready": True, "message": "ready"},
    )
    require(ready["ready"] is True, "ready status receipt lost")

    dead_process = FakeProcess(returncode=17)
    require_raises(
        WebDriverRuntimeLaunchError,
        lambda: wait_for_webdriver_ready(
            dead_process,
            "http://127.0.0.1:47007",
            timeout_seconds=0.1,
            interval_seconds=0.001,
            status_reader=lambda _url: {"ready": True},
        ),
        "dead engine process was accepted as ready",
    )

    prepared = PreparedWebDriverRuntime(
        competitor="servo",
        binary="/fake/servo",
        port=47008,
        command=("/fake/servo", "--webdriver=47008"),
        webdriver_url="http://127.0.0.1:47008",
        runtime_identity="servo-test-identity",
        identity_source="test",
    )
    spawned: list[tuple[tuple[str, ...], dict[str, object]]] = []
    launched_process = FakeProcess(timeout_on_first_wait=True)

    def fake_popen(command, **kwargs):
        spawned.append((tuple(command), dict(kwargs)))
        return launched_process

    handle = launch_prepared_runtime(
        prepared,
        startup_timeout_seconds=0.2,
        popen_factory=fake_popen,
        status_reader=lambda _url: {"ready": True, "message": "ready"},
    )
    require(spawned[0][0] == prepared.command, "runtime spawned the wrong command")
    require(handle.status_receipt["ready"] is True, "runtime status receipt lost")
    handle.close(grace_seconds=0.01)
    require(launched_process.terminated, "runtime process was not terminated")
    require(launched_process.killed, "runtime process timeout did not escalate to kill")
    handle.close(grace_seconds=0.01)

    failing_process = FakeProcess()
    require_raises(
        WebDriverRuntimeLaunchError,
        lambda: launch_prepared_runtime(
            prepared,
            startup_timeout_seconds=0.01,
            popen_factory=lambda *_args, **_kwargs: failing_process,
            status_reader=lambda _url: {"ready": False, "message": "not ready"},
        ),
        "never-ready WebDriver runtime was admitted",
    )
    require(failing_process.terminated, "failed runtime launch did not terminate process")

    print("Zevryon WebDriver runtime authority tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
