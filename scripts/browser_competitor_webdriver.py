#!/usr/bin/env python3
from __future__ import annotations

from dataclasses import dataclass
import json
import socket
import time
from typing import Any, Mapping
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen


class WebDriverTransportError(RuntimeError):
    pass


class WebDriverProtocolError(RuntimeError):
    pass


def _normalize_base_url(base_url: str) -> str:
    normalized = base_url.rstrip("/")
    if not normalized.startswith(("http://", "https://")):
        raise ValueError("WebDriver base URL must use http or https")
    return normalized


def _decode_response(raw: bytes, *, context: str) -> dict[str, Any]:
    try:
        decoded = json.loads(raw.decode("utf-8")) if raw else {"value": None}
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise WebDriverTransportError(f"WebDriver returned invalid JSON for {context}") from exc
    if not isinstance(decoded, dict):
        raise WebDriverTransportError("WebDriver response root must be a JSON object")
    return decoded


def _raise_protocol_error_if_present(decoded: Mapping[str, Any]) -> None:
    value = decoded.get("value")
    if isinstance(value, dict) and isinstance(value.get("error"), str):
        message = str(value.get("message") or value["error"])
        raise WebDriverProtocolError(f"{value['error']}: {message}")


def wait_for_tcp(
    host: str,
    port: int,
    *,
    timeout_seconds: float,
    interval_seconds: float = 0.05,
) -> None:
    if not host:
        raise ValueError("host cannot be blank")
    if not isinstance(port, int) or isinstance(port, bool) or port < 1 or port > 65535:
        raise ValueError("port must be in 1..65535")
    if timeout_seconds <= 0 or interval_seconds <= 0:
        raise ValueError("timeouts must be positive")

    deadline = time.monotonic() + timeout_seconds
    last_error: OSError | None = None
    while time.monotonic() < deadline:
        try:
            with socket.create_connection((host, port), timeout=min(0.5, timeout_seconds)):
                return
        except OSError as exc:
            last_error = exc
            time.sleep(interval_seconds)
    raise WebDriverTransportError(
        f"WebDriver endpoint {host}:{port} did not become reachable within "
        f"{timeout_seconds:.3f}s: {last_error}"
    )


def request_json(
    base_url: str,
    method: str,
    path: str,
    payload: Mapping[str, Any] | None = None,
    *,
    timeout_seconds: float = 30.0,
) -> dict[str, Any]:
    if timeout_seconds <= 0:
        raise ValueError("timeout_seconds must be positive")
    normalized_base = _normalize_base_url(base_url)
    normalized_path = path if path.startswith("/") else f"/{path}"
    context = f"{method.upper()} {normalized_path}"
    data = None
    headers = {"Accept": "application/json"}
    if payload is not None:
        data = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        headers["Content-Type"] = "application/json"
    request = Request(
        normalized_base + normalized_path,
        data=data,
        headers=headers,
        method=method.upper(),
    )
    try:
        with urlopen(request, timeout=timeout_seconds) as response:
            raw = response.read()
    except HTTPError as exc:
        raw = exc.read()
        decoded = _decode_response(raw, context=context)
        _raise_protocol_error_if_present(decoded)
        raise WebDriverTransportError(
            f"WebDriver HTTP {exc.code} for {context}: "
            f"{raw.decode('utf-8', errors='replace')}"
        ) from exc
    except (URLError, TimeoutError, OSError) as exc:
        raise WebDriverTransportError(f"WebDriver transport failure for {context}: {exc}") from exc

    decoded = _decode_response(raw, context=context)
    _raise_protocol_error_if_present(decoded)
    return decoded


def webdriver_status(base_url: str, *, timeout_seconds: float = 5.0) -> dict[str, Any]:
    response = request_json(
        base_url,
        "GET",
        "/status",
        timeout_seconds=timeout_seconds,
    )
    value = response.get("value")
    if not isinstance(value, dict):
        raise WebDriverProtocolError("status response lacks value object")
    return dict(value)


@dataclass
class WebDriverSession:
    base_url: str
    session_id: str
    capabilities: dict[str, Any]
    timeout_seconds: float = 30.0
    _closed: bool = False

    @classmethod
    def create(
        cls,
        base_url: str,
        *,
        always_match: Mapping[str, Any] | None = None,
        timeout_seconds: float = 30.0,
    ) -> "WebDriverSession":
        response = request_json(
            base_url,
            "POST",
            "/session",
            {"capabilities": {"alwaysMatch": dict(always_match or {})}},
            timeout_seconds=timeout_seconds,
        )
        value = response.get("value")
        if not isinstance(value, dict):
            raise WebDriverProtocolError("new session response lacks value object")
        session_id = value.get("sessionId")
        capabilities = value.get("capabilities", {})
        if not isinstance(session_id, str) or not session_id.strip():
            raise WebDriverProtocolError("new session response lacks sessionId")
        if not isinstance(capabilities, dict):
            raise WebDriverProtocolError("new session capabilities must be an object")
        return cls(
            base_url=_normalize_base_url(base_url),
            session_id=session_id.strip(),
            capabilities=dict(capabilities),
            timeout_seconds=timeout_seconds,
        )

    def _path(self, suffix: str = "") -> str:
        suffix = suffix if not suffix or suffix.startswith("/") else f"/{suffix}"
        return f"/session/{self.session_id}{suffix}"

    def set_timeouts(self, *, script_ms: int, page_load_ms: int) -> None:
        if script_ms <= 0 or page_load_ms <= 0:
            raise ValueError("WebDriver timeouts must be positive")
        request_json(
            self.base_url,
            "POST",
            self._path("/timeouts"),
            {"script": int(script_ms), "pageLoad": int(page_load_ms)},
            timeout_seconds=self.timeout_seconds,
        )

    def set_window_rect(self, *, width: int, height: int) -> dict[str, Any]:
        if (
            not isinstance(width, int)
            or isinstance(width, bool)
            or width <= 0
            or not isinstance(height, int)
            or isinstance(height, bool)
            or height <= 0
        ):
            raise ValueError("window dimensions must be positive integers")
        response = request_json(
            self.base_url,
            "POST",
            self._path("/window/rect"),
            {"width": width, "height": height},
            timeout_seconds=self.timeout_seconds,
        )
        value = response.get("value")
        if value is None:
            return {}
        if not isinstance(value, dict):
            raise WebDriverProtocolError("set window rect response value must be an object")
        return dict(value)

    def navigate(self, url: str) -> None:
        if not url:
            raise ValueError("navigation URL cannot be blank")
        request_json(
            self.base_url,
            "POST",
            self._path("/url"),
            {"url": url},
            timeout_seconds=self.timeout_seconds,
        )

    def execute_sync(self, script: str, args: list[Any] | None = None) -> Any:
        if not script:
            raise ValueError("script cannot be blank")
        response = request_json(
            self.base_url,
            "POST",
            self._path("/execute/sync"),
            {"script": script, "args": list(args or [])},
            timeout_seconds=self.timeout_seconds,
        )
        return response.get("value")

    def close(self) -> None:
        if self._closed:
            return
        request_json(
            self.base_url,
            "DELETE",
            self._path(),
            timeout_seconds=self.timeout_seconds,
        )
        self._closed = True

    def __enter__(self) -> "WebDriverSession":
        return self

    def __exit__(self, exc_type, exc, traceback) -> None:
        try:
            self.close()
        except (WebDriverTransportError, WebDriverProtocolError):
            if exc is None:
                raise
