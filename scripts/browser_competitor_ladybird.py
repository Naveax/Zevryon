#!/usr/bin/env python3
from __future__ import annotations

from dataclasses import dataclass
import hashlib
import os
from pathlib import Path
import shutil
from typing import Callable, Mapping


LADYBIRD_WEBDRIVER_BINARY_ENV = "ZEVRYON_LADYBIRD_WEBDRIVER_BIN"


class LadybirdAdapterUnavailable(RuntimeError):
    pass


class LadybirdAdapterInvalid(RuntimeError):
    pass


@dataclass(frozen=True)
class LadybirdLaunchPlan:
    binary: str
    port: int
    command: tuple[str, ...]
    webdriver_url: str


def resolve_ladybird_webdriver_binary(
    explicit: str | None = None,
    *,
    environment: Mapping[str, str] | None = None,
    which: Callable[[str], str | None] = shutil.which,
) -> str:
    env = os.environ if environment is None else environment
    candidate = explicit or env.get(LADYBIRD_WEBDRIVER_BINARY_ENV)
    if candidate:
        return str(Path(candidate).expanduser())

    discovered = which("WebDriver")
    if discovered:
        return str(Path(discovered).expanduser())

    raise LadybirdAdapterUnavailable(
        "Ladybird WebDriver binary is unavailable; pass an explicit path, set "
        f"{LADYBIRD_WEBDRIVER_BINARY_ENV}, or put WebDriver on PATH"
    )


def build_ladybird_launch_plan(binary: str, port: int) -> LadybirdLaunchPlan:
    normalized_binary = str(Path(binary).expanduser())
    if not normalized_binary.strip():
        raise LadybirdAdapterInvalid("Ladybird WebDriver binary path cannot be blank")
    if not isinstance(port, int) or isinstance(port, bool) or port < 1 or port > 65535:
        raise LadybirdAdapterInvalid("Ladybird WebDriver port must be in 1..65535")

    return LadybirdLaunchPlan(
        binary=normalized_binary,
        port=port,
        command=(
            normalized_binary,
            "--headless",
            "-l",
            "127.0.0.1",
            "-p",
            str(port),
        ),
        webdriver_url=f"http://127.0.0.1:{port}",
    )


def binary_sha256(binary: str | Path) -> str:
    path = Path(binary)
    digest = hashlib.sha256()
    try:
        with path.open("rb") as handle:
            while True:
                chunk = handle.read(1024 * 1024)
                if not chunk:
                    break
                digest.update(chunk)
    except (FileNotFoundError, PermissionError, OSError) as exc:
        raise LadybirdAdapterUnavailable(
            f"cannot hash Ladybird WebDriver binary {path}: {exc}"
        ) from exc
    return digest.hexdigest()


def runtime_identity(plan: LadybirdLaunchPlan, binary_digest: str) -> str:
    digest = binary_digest.strip().lower()
    if len(digest) != 64 or any(character not in "0123456789abcdef" for character in digest):
        raise LadybirdAdapterInvalid("Ladybird binary identity requires lowercase SHA-256")
    return (
        f"ladybird|webdriver_binary={plan.binary}|sha256={digest}|"
        f"webdriver=127.0.0.1:{plan.port}|headless=true"
    )
