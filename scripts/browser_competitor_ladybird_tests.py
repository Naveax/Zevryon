#!/usr/bin/env python3
from __future__ import annotations

from pathlib import Path
import tempfile

from browser_competitor_ladybird import (
    LADYBIRD_WEBDRIVER_BINARY_ENV,
    LadybirdAdapterInvalid,
    LadybirdAdapterUnavailable,
    binary_sha256,
    build_ladybird_launch_plan,
    resolve_ladybird_webdriver_binary,
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
        resolve_ladybird_webdriver_binary(
            "/opt/ladybird/WebDriver", environment={}, which=lambda _name: None
        )
        == "/opt/ladybird/WebDriver",
        "explicit Ladybird WebDriver path lost",
    )
    require(
        resolve_ladybird_webdriver_binary(
            None,
            environment={LADYBIRD_WEBDRIVER_BINARY_ENV: "/env/WebDriver"},
            which=lambda _name: None,
        )
        == "/env/WebDriver",
        "Ladybird environment path lost",
    )
    require(
        resolve_ladybird_webdriver_binary(
            None,
            environment={},
            which=lambda name: "/usr/bin/WebDriver" if name == "WebDriver" else None,
        )
        == "/usr/bin/WebDriver",
        "Ladybird WebDriver PATH resolution failed",
    )
    require_raises(
        LadybirdAdapterUnavailable,
        lambda: resolve_ladybird_webdriver_binary(
            None, environment={}, which=lambda _name: None
        ),
        "missing Ladybird WebDriver binary was accepted",
    )

    plan = build_ladybird_launch_plan("/opt/ladybird/WebDriver", 4540)
    require(
        plan.command
        == (
            "/opt/ladybird/WebDriver",
            "--headless",
            "-l",
            "127.0.0.1",
            "-p",
            "4540",
        ),
        "Ladybird launch command drifted",
    )
    require(
        plan.webdriver_url == "http://127.0.0.1:4540",
        "Ladybird WebDriver URL drifted",
    )

    expected_digest = "0165b131c9ccec65003f9438744170796df29dc98ed2ed3cc4cfb00a03484320"
    with tempfile.TemporaryDirectory() as directory:
        binary = Path(directory) / "WebDriver"
        binary.write_bytes(b"ladybird-test-binary-v1")
        digest = binary_sha256(binary)
        require(digest == expected_digest, "Ladybird binary SHA-256 drifted")
        identity = runtime_identity(plan, digest)
        require(
            identity
            == f"ladybird|webdriver_binary=/opt/ladybird/WebDriver|sha256={expected_digest}|webdriver=127.0.0.1:4540|headless=true",
            "Ladybird runtime identity drifted",
        )

    require_raises(
        LadybirdAdapterUnavailable,
        lambda: binary_sha256("/definitely/missing/WebDriver"),
        "missing Ladybird binary was hashed successfully",
    )
    for bad_port in (0, -1, 65536, True):
        require_raises(
            LadybirdAdapterInvalid,
            lambda bad_port=bad_port: build_ladybird_launch_plan(
                "/opt/ladybird/WebDriver", bad_port
            ),
            f"invalid Ladybird port accepted: {bad_port!r}",
        )
    require_raises(
        LadybirdAdapterInvalid,
        lambda: runtime_identity(plan, "not-a-sha"),
        "invalid Ladybird binary digest was accepted",
    )

    print("Zevryon Ladybird adapter tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
