#!/usr/bin/env python3
from __future__ import annotations

from browser_competitor_scenario_contract import (
    SYNTHETIC_PATTERN,
    VIEWPORT_HEIGHT,
    VIEWPORT_WIDTH,
)
from browser_competitor_webdriver_scenario import (
    CREATE_BLOB_SCRIPT,
    GC_SCRIPT,
    INNER_VIEWPORT_SCRIPT,
    INSTALL_DOCUMENT_SCRIPT,
    NATIVE_QUERY_ASYNC_SCRIPT,
    NATIVE_SETUP_ASYNC_SCRIPT,
    PAYLOAD_PATTERN_TEXT,
    VIRTUAL_QUERY_ASYNC_SCRIPT,
    WEBDRIVER_SCENARIO_POLICY,
    WebDriverScenarioInvalid,
    enforce_inner_viewport,
    request_gc,
    setup_webdriver_scenario,
    webdriver_native_query,
    webdriver_virtual_query,
)


class FakeSession:
    def __init__(self) -> None:
        self.outer_width = VIEWPORT_WIDTH
        self.outer_height = VIEWPORT_HEIGHT
        self.rect_calls: list[tuple[int, int]] = []
        self.navigate_calls: list[str] = []
        self.sync_calls: list[tuple[str, list[object]]] = []
        self.async_calls: list[tuple[str, list[object]]] = []

    def set_window_rect(self, *, width: int, height: int) -> dict[str, int]:
        self.outer_width = width
        self.outer_height = height
        self.rect_calls.append((width, height))
        return {"x": 0, "y": 0, "width": width, "height": height}

    def navigate(self, url: str) -> None:
        self.navigate_calls.append(url)

    def execute_sync(self, script: str, args: list[object] | None = None):
        materialized = list(args or [])
        self.sync_calls.append((script, materialized))
        if script == INNER_VIEWPORT_SCRIPT:
            return {
                "width": self.outer_width - 20,
                "height": self.outer_height - 40,
            }
        if script == INSTALL_DOCUMENT_SCRIPT:
            return True
        if script == CREATE_BLOB_SCRIPT:
            return {
                "blob_bytes": int(materialized[0]),
                "pattern_bytes": len(SYNTHETIC_PATTERN),
            }
        if script == GC_SCRIPT:
            return None
        raise AssertionError("unexpected sync script")

    def execute_async(self, script: str, args: list[object] | None = None):
        materialized = list(args or [])
        self.async_calls.append((script, materialized))
        if script == NATIVE_SETUP_ASYNC_SCRIPT:
            return {
                "ok": True,
                "value": {
                    "setup_milliseconds": 12.5,
                    "decoded_utf16_units": 12345,
                    "scroll_height": 67890,
                },
            }
        if script == VIRTUAL_QUERY_ASYNC_SCRIPT:
            return {
                "ok": True,
                "value": {
                    "milliseconds": 1.75,
                    "rendered_utf16_units": 2048,
                    "rendered_height": 432,
                },
            }
        if script == NATIVE_QUERY_ASYNC_SCRIPT:
            return {"ok": True, "value": 2.25}
        raise AssertionError("unexpected async script")


class BadBlobSession(FakeSession):
    def execute_sync(self, script: str, args: list[object] | None = None):
        if script == CREATE_BLOB_SCRIPT:
            materialized = list(args or [])
            return {
                "blob_bytes": int(materialized[0]) - 1,
                "pattern_bytes": len(SYNTHETIC_PATTERN),
            }
        return super().execute_sync(script, args)


class AsyncFailureSession(FakeSession):
    def execute_async(self, script: str, args: list[object] | None = None):
        if script == VIRTUAL_QUERY_ASYNC_SCRIPT:
            return {"ok": False, "error": "synthetic failure"}
        return super().execute_async(script, args)


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
        PAYLOAD_PATTERN_TEXT.encode("utf-8") == SYNTHETIC_PATTERN,
        "WebDriver payload pattern drifted from canonical corpus generator",
    )
    require(
        WEBDRIVER_SCENARIO_POLICY == "w3c-async-callback-double-raf-inner-viewport-v1",
        "WebDriver scenario policy drifted",
    )

    virtual = FakeSession()
    virtual_setup = setup_webdriver_scenario(
        virtual,
        mode="virtualized",
        payload_bytes=8 * 1024 * 1024,
    )
    require(virtual.navigate_calls == ["about:blank"], "virtual scenario navigation drifted")
    require(
        virtual.rect_calls == [(800, 720), (820, 760)],
        "inner viewport calibration sequence drifted",
    )
    require(
        virtual_setup["viewport"]["attempts"][-1]["inner_width"] == 800
        and virtual_setup["viewport"]["attempts"][-1]["inner_height"] == 720,
        "exact inner viewport was not admitted",
    )
    require(
        virtual_setup["blob_bytes"] == 8 * 1024 * 1024,
        "virtual blob identity drifted",
    )
    detail = webdriver_virtual_query(virtual, offset=4096, slice_bytes=131072)
    require(detail["milliseconds"] == 1.75, "virtual query timing drifted")
    require(
        virtual.async_calls[-1] == (VIRTUAL_QUERY_ASYNC_SCRIPT, [4096, 131072]),
        "virtual query async argument order drifted",
    )
    request_gc(virtual)
    require(virtual.sync_calls[-1][0] == GC_SCRIPT, "GC request drifted")

    native = FakeSession()
    native_setup = setup_webdriver_scenario(
        native,
        mode="native-dom",
        payload_bytes=4 * 1024 * 1024,
    )
    require(native_setup["setup_milliseconds"] == 12.5, "native setup timing lost")
    require(
        native.async_calls[0] == (NATIVE_SETUP_ASYNC_SCRIPT, []),
        "native setup did not use async WebDriver execution",
    )
    native_ms = webdriver_native_query(native, fraction=0.625)
    require(native_ms == 2.25, "native query timing drifted")
    require(
        native.async_calls[-1] == (NATIVE_QUERY_ASYNC_SCRIPT, [0.625]),
        "native query async argument drifted",
    )

    stuck = FakeSession()
    stuck.outer_width = 10
    stuck.outer_height = 10
    require_raises(
        WebDriverScenarioInvalid,
        lambda: enforce_inner_viewport(stuck, max_attempts=1),
        "non-matching inner viewport was admitted",
    )
    require_raises(
        WebDriverScenarioInvalid,
        lambda: setup_webdriver_scenario(
            BadBlobSession(),
            mode="virtualized",
            payload_bytes=1024,
        ),
        "blob byte mismatch was admitted",
    )
    require_raises(
        WebDriverScenarioInvalid,
        lambda: webdriver_virtual_query(
            AsyncFailureSession(),
            offset=0,
            slice_bytes=1,
        ),
        "async JavaScript failure was admitted",
    )
    require_raises(
        ValueError,
        lambda: setup_webdriver_scenario(FakeSession(), mode="unknown", payload_bytes=1),
        "unknown mode was accepted",
    )
    require_raises(
        ValueError,
        lambda: webdriver_native_query(FakeSession(), fraction=1.1),
        "out-of-range native scroll fraction was accepted",
    )

    print("Zevryon WebDriver common-scenario tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
