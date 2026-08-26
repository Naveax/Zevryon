#!/usr/bin/env python3
from __future__ import annotations

from dataclasses import dataclass
import os
from pathlib import Path
import shutil
from typing import Callable, Mapping


SERVO_BINARY_ENV = "ZEVRYON_SERVO_BIN"


class ServoAdapterUnavailable(RuntimeError):
    pass


class ServoAdapterInvalid(RuntimeError):
    pass


@dataclass(frozen=True)
class ServoLaunchPlan:
    binary: str
    port: int
    command: tuple[str, ...]
    webdriver_url: str
    identity_command: tuple[str, ...]


def resolve_servo_binary(
    explicit: str | None = None,
    *,
    environment: Mapping[str, str] | None = None,
    which: Callable[[str], str | None] = shutil.which,
) -> str:
    env = os.environ if environment is None else environment
    candidate = explicit or env.get(SERVO_BINARY_ENV)
    if candidate:
        return str(Path(candidate).expanduser())

    discovered = which("servo") or which("servoshell")
    if discovered:
        return str(Path(discovered).expanduser())

    raise ServoAdapterUnavailable(
        "Servo binary is unavailable; pass an explicit path, set "
        f"{SERVO_BINARY_ENV}, or install servo/servoshell on PATH"
    )


def build_servo_launch_plan(binary: str, port: int) -> ServoLaunchPlan:
    normalized_binary = str(Path(binary).expanduser())
    if not normalized_binary.strip():
        raise ServoAdapterInvalid("Servo binary path cannot be blank")
    if not isinstance(port, int) or isinstance(port, bool) or port < 1 or port > 65535:
        raise ServoAdapterInvalid("Servo WebDriver port must be in 1..65535")

    return ServoLaunchPlan(
        binary=normalized_binary,
        port=port,
        command=(
            normalized_binary,
            "--headless",
            f"--webdriver={port}",
            "about:blank",
        ),
        webdriver_url=f"http://127.0.0.1:{port}",
        identity_command=(normalized_binary, "--version"),
    )


def parse_servo_version(stdout: str, stderr: str = "") -> str:
    for stream in (stdout, stderr):
        for line in stream.splitlines():
            normalized = line.strip()
            if normalized:
                return normalized
    raise ServoAdapterInvalid("Servo --version returned no identity text")


def runtime_identity(plan: ServoLaunchPlan, version_text: str) -> str:
    version = version_text.strip()
    if not version:
        raise ServoAdapterInvalid("Servo runtime identity requires version text")
    return (
        f"servo|binary={plan.binary}|version={version}|"
        f"webdriver=127.0.0.1:{plan.port}|headless=true"
    )
