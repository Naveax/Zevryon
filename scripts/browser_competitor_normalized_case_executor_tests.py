#!/usr/bin/env python3
from __future__ import annotations

from browser_competitor_scenario_contract import ScenarioContractInvalid
import browser_competitor_normalized_case_executor as module


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def require_raises(exc_type, callable_, message: str) -> None:
    try:
        callable_()
    except exc_type:
        return
    raise AssertionError(message)


class FakePage:
    def __init__(self, virtual_result=None, native_result=2.0) -> None:
        self.virtual_result = (
            {"milliseconds": 1.5, "rendered_utf16_units": 12, "rendered_height": 18}
            if virtual_result is None
            else virtual_result
        )
        self.native_result = native_result

    def evaluate(self, script, argument):
        if "__renderVirtualSlice" in script:
            return self.virtual_result
        if "__scrollNative" in script:
            return self.native_result
        raise AssertionError("unexpected page query script")


class FakeQueue:
    def __init__(self) -> None:
        self.values: list[object] = []

    def put(self, value: object) -> None:
        self.values.append(value)


def main() -> int:
    virtual_ms, virtual_detail = module._playwright_query(
        FakePage(),
        mode="virtualized",
        offset=123,
        slice_bytes=456,
    )
    require(virtual_ms == 1.5, "Playwright virtual timing drifted")
    require(virtual_detail["byte_offset"] == 123, "Playwright virtual offset lost")
    require(
        virtual_detail["rendered_utf16_units"] == 12,
        "Playwright virtual detail lost",
    )

    native_ms, native_detail = module._playwright_query(
        FakePage(native_result=3.25),
        mode="native-dom",
        offset=500_000,
        slice_bytes=999,
    )
    require(native_ms == 3.25, "Playwright native timing drifted")
    require(native_detail["scroll_fraction"] == 0.5, "Playwright native fraction drifted")
    require(native_detail["scroll_offset_unit"] == 500_000, "Playwright native offset lost")

    require_raises(
        ScenarioContractInvalid,
        lambda: module._playwright_query(
            FakePage(virtual_result={"milliseconds": -1.0}),
            mode="virtualized",
            offset=0,
            slice_bytes=1,
        ),
        "negative Playwright virtual timing was accepted",
    )
    require_raises(
        ScenarioContractInvalid,
        lambda: module._playwright_query(
            FakePage(virtual_result="bad"),
            mode="virtualized",
            offset=0,
            slice_bytes=1,
        ),
        "non-object Playwright virtual result was accepted",
    )

    original_virtual = module.webdriver_virtual_query
    original_native = module.webdriver_native_query
    original_playwright_case = module._playwright_case
    original_webdriver_case = module._webdriver_case
    original_execute = module.execute_normalized_case
    try:
        module.webdriver_virtual_query = lambda _session, *, offset, slice_bytes: {
            "milliseconds": 4.5,
            "offset": offset,
            "slice": slice_bytes,
        }
        module.webdriver_native_query = lambda _session, *, fraction: 5.5 + fraction

        webdriver_ms, webdriver_detail = module._webdriver_query(
            object(),
            mode="virtualized",
            offset=7,
            slice_bytes=9,
        )
        require(webdriver_ms == 4.5, "WebDriver virtual timing drifted")
        require(webdriver_detail["byte_offset"] == 7, "WebDriver virtual offset lost")
        require(webdriver_detail["slice"] == 9, "WebDriver virtual slice lost")

        native_wd_ms, native_wd_detail = module._webdriver_query(
            object(),
            mode="native-dom",
            offset=250_000,
            slice_bytes=1,
        )
        require(native_wd_ms == 5.75, "WebDriver native timing drifted")
        require(native_wd_detail["scroll_fraction"] == 0.25, "WebDriver native fraction drifted")

        module._playwright_case = lambda *args: {"route": "playwright", "args": args}
        module._webdriver_case = lambda *args: {"route": "webdriver", "args": args}
        routed_playwright = module.execute_normalized_case(
            adapter="playwright",
            competitor="chrome",
            mode="virtualized",
            payload_bytes=1,
            query_count=2,
            warmup_query_count=3,
            slice_bytes=1,
            case_timeout_seconds=1,
            evidence_kwargs={},
        )
        require(routed_playwright["route"] == "playwright", "normalized Playwright route drifted")
        routed_webdriver = module.execute_normalized_case(
            adapter="webdriver",
            competitor="servo",
            mode="native-dom",
            payload_bytes=1,
            query_count=2,
            warmup_query_count=3,
            slice_bytes=1,
            case_timeout_seconds=1,
            evidence_kwargs={},
        )
        require(routed_webdriver["route"] == "webdriver", "normalized WebDriver route drifted")

        unsupported = module.execute_normalized_case(
            adapter="future",
            competitor="servo",
            mode="virtualized",
            payload_bytes=1,
            query_count=1,
            warmup_query_count=0,
            slice_bytes=1,
            case_timeout_seconds=1,
            evidence_kwargs={},
        )
        require(unsupported["status"] == "unsupported", "unknown normalized adapter was accepted")

        queue = FakeQueue()
        module.execute_normalized_case = lambda **_kwargs: (_ for _ in ()).throw(
            RuntimeError("normalized worker boom")
        )
        module.normalized_worker_entry(
            "webdriver",
            "servo",
            "virtualized",
            1,
            1,
            0,
            1,
            1,
            {},
            queue,
        )
        require(len(queue.values) == 1, "normalized worker did not emit exactly one result")
        worker_result = queue.values[0]
        require(isinstance(worker_result, dict), "normalized worker result is not a mapping")
        require(worker_result["status"] == "error", "normalized worker exception not serialized")
        require(
            "normalized worker boom" in str(worker_result["reason"]),
            "normalized worker exception reason lost",
        )
    finally:
        module.webdriver_virtual_query = original_virtual
        module.webdriver_native_query = original_native
        module._playwright_case = original_playwright_case
        module._webdriver_case = original_webdriver_case
        module.execute_normalized_case = original_execute

    print("normalized browser case executor tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
