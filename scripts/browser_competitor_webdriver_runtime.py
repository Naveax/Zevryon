#!/usr/bin/env python3
from __future__ import annotations

from dataclasses import dataclass
import os
import shutil
import socket
import subprocess
import time
from typing import Any, Callable, Mapping

from browser_competitor_ladybird import (
    LadybirdAdapterInvalid,
    LadybirdAdapterUnavailable,
    binary_sha256 as ladybird_binary_sha256,
    build_ladybird_launch_plan,
    resolve_ladybird_webdriver_binary,
    runtime_identity as ladybird_runtime_identity,
)
from browser_competitor_servo import (
    ServoAdapterInvalid,
    ServoAdapterUnavailable,
    build_servo_launch_plan,
    parse_servo_version,
    resolve_servo_binary,
    runtime_identity as servo_runtime_identity,
)
from browser_competitor_webdriver import (
    WebDriverProtocolError,
    WebDriverTransportError,
    webdriver_status,
)


class WebDriverRuntimeUnavailable(RuntimeError):
    pass


class WebDriverRuntimeInvalid(RuntimeError):
    pass


class WebDriverRuntimeLaunchError(RuntimeError):
    pass


IdentityRunner = Callable[[tuple[str, ...]], tuple[int, str, str]]
BinaryHasher = Callable[[str], str]
StatusReader = Callable[[str], dict[str, Any]]
PopenFactory = Callable[..., Any]


@dataclass(frozen=True)
class PreparedWebDriverRuntime:
    competitor: str
    binary: str
    port: int
    command: tuple[str, ...]
    webdriver_url: str
    runtime_identity: str
    identity_source: str


@dataclass
class WebDriverEngineHandle:
    prepared: PreparedWebDriverRuntime
    process: Any
    status_receipt: dict[str, Any]
    _closed: bool = False

    def close(self, *, grace_seconds: float = 5.0) -> None:
        if self._closed:
            return
        if grace_seconds <= 0:
            raise ValueError("grace_seconds must be positive")
        self._closed = True
        if self.process.poll() is not None:
            return
        self.process.terminate()
        try:
            self.process.wait(timeout=grace_seconds)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=grace_seconds)

    def __enter__(self) -> "WebDriverEngineHandle":
        return self

    def __exit__(self, exc_type, exc, traceback) -> None:
        self.close()


def allocate_loopback_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as handle:
        handle.bind(("127.0.0.1", 0))
        port = int(handle.getsockname()[1])
    if port < 1 or port > 65535:
        raise WebDriverRuntimeInvalid(f"invalid ephemeral port allocated: {port}")
    return port


def _default_identity_runner(command: tuple[str, ...]) -> tuple[int, str, str]:
    completed = subprocess.run(
        command,
        capture_output=True,
        text=True,
        timeout=15.0,
        check=False,
    )
    return int(completed.returncode), completed.stdout, completed.stderr


def prepare_webdriver_runtime(
    competitor: str,
    *,
    port: int | None = None,
    explicit_binary: str | None = None,
    environment: Mapping[str, str] | None = None,
    which: Callable[[str], str | None] = shutil.which,
    identity_runner: IdentityRunner = _default_identity_runner,
    ladybird_hasher: BinaryHasher = lambda path: ladybird_binary_sha256(path),
) -> PreparedWebDriverRuntime:
    selected_port = allocate_loopback_port() if port is None else port
    if not isinstance(selected_port, int) or isinstance(selected_port, bool) or not 1 <= selected_port <= 65535:
        raise ValueError("WebDriver runtime port must be in 1..65535")

    if competitor == "servo":
        try:
            binary = resolve_servo_binary(
                explicit_binary,
                environment=os.environ if environment is None else environment,
                which=which,
            )
            launch = build_servo_launch_plan(binary, selected_port)
            returncode, stdout, stderr = identity_runner(launch.identity_command)
        except (ServoAdapterUnavailable, FileNotFoundError, PermissionError, OSError) as exc:
            raise WebDriverRuntimeUnavailable(str(exc)) from exc
        except (ServoAdapterInvalid, subprocess.TimeoutExpired) as exc:
            raise WebDriverRuntimeInvalid(str(exc)) from exc
        if returncode != 0:
            detail = (stderr or stdout).strip()
            raise WebDriverRuntimeInvalid(
                f"Servo identity command exited with code {returncode}"
                + (f": {detail}" if detail else "")
            )
        try:
            version = parse_servo_version(stdout, stderr)
            identity = servo_runtime_identity(launch, version)
        except ServoAdapterInvalid as exc:
            raise WebDriverRuntimeInvalid(str(exc)) from exc
        return PreparedWebDriverRuntime(
            competitor="servo",
            binary=launch.binary,
            port=launch.port,
            command=launch.command,
            webdriver_url=launch.webdriver_url,
            runtime_identity=identity,
            identity_source="servo --version",
        )

    if competitor == "ladybird":
        try:
            binary = resolve_ladybird_webdriver_binary(
                explicit_binary,
                environment=os.environ if environment is None else environment,
                which=which,
            )
            launch = build_ladybird_launch_plan(binary, selected_port)
            digest = ladybird_hasher(launch.binary)
            identity = ladybird_runtime_identity(launch, digest)
        except (LadybirdAdapterUnavailable, FileNotFoundError, PermissionError, OSError) as exc:
            raise WebDriverRuntimeUnavailable(str(exc)) from exc
        except LadybirdAdapterInvalid as exc:
            raise WebDriverRuntimeInvalid(str(exc)) from exc
        return PreparedWebDriverRuntime(
            competitor="ladybird",
            binary=launch.binary,
            port=launch.port,
            command=launch.command,
            webdriver_url=launch.webdriver_url,
            runtime_identity=identity,
            identity_source="exact WebDriver binary SHA-256",
        )

    raise ValueError(f"WebDriver runtime does not support competitor: {competitor}")


def wait_for_webdriver_ready(
    process: Any,
    webdriver_url: str,
    *,
    timeout_seconds: float = 20.0,
    interval_seconds: float = 0.05,
    status_reader: StatusReader = lambda url: webdriver_status(url, timeout_seconds=0.5),
) -> dict[str, Any]:
    if timeout_seconds <= 0 or interval_seconds <= 0:
        raise ValueError("runtime readiness timeouts must be positive")
    deadline = time.monotonic() + timeout_seconds
    last_reason = "endpoint not contacted"
    while time.monotonic() < deadline:
        returncode = process.poll()
        if returncode is not None:
            raise WebDriverRuntimeLaunchError(
                f"WebDriver engine exited before readiness with code {returncode}"
            )
        try:
            status = status_reader(webdriver_url)
            if status.get("ready") is True:
                return dict(status)
            last_reason = str(status.get("message") or "WebDriver status ready=false")
        except (WebDriverTransportError, WebDriverProtocolError, OSError) as exc:
            last_reason = str(exc)
        time.sleep(interval_seconds)
    raise WebDriverRuntimeLaunchError(
        f"WebDriver endpoint did not become ready within {timeout_seconds:.3f}s: {last_reason}"
    )


def launch_prepared_runtime(
    prepared: PreparedWebDriverRuntime,
    *,
    startup_timeout_seconds: float = 20.0,
    popen_factory: PopenFactory = subprocess.Popen,
    status_reader: StatusReader = lambda url: webdriver_status(url, timeout_seconds=0.5),
) -> WebDriverEngineHandle:
    if startup_timeout_seconds <= 0:
        raise ValueError("startup_timeout_seconds must be positive")
    process = popen_factory(
        prepared.command,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    try:
        status = wait_for_webdriver_ready(
            process,
            prepared.webdriver_url,
            timeout_seconds=startup_timeout_seconds,
            status_reader=status_reader,
        )
    except BaseException:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=2.0)
        raise
    return WebDriverEngineHandle(
        prepared=prepared,
        process=process,
        status_receipt=status,
    )
