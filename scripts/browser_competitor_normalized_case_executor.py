#!/usr/bin/env python3
from __future__ import annotations

import os
import time
import traceback
from typing import Any

from browser_competitor_case_executor import (
    _playwright_setup_native,
    _playwright_setup_page,
    _playwright_setup_virtualized,
    _query_summary,
    failure_case_record,
    process_scope_metrics,
    webdriver_failure_status,
)
from browser_competitor_normalized_browser_lifecycle import (
    BrowserNormalizedLifecycleInvalid,
    attach_browser_normalized_evidence,
)
from browser_competitor_playwright import (
    launch_browser,
    launch_failure_status,
    runtime_identity as playwright_runtime_identity,
)
from browser_competitor_process_scope import (
    BrowserProcessScopeMonitor,
    ProcessScopeUnavailable,
    descendant_identities,
)
from browser_competitor_query_plan import native_fraction, plan_query_offsets
from browser_competitor_registry import get_spec, terminal_record
from browser_competitor_scenario_contract import (
    VIEWPORT_HEIGHT,
    VIEWPORT_WIDTH,
    ScenarioContractInvalid,
)
from browser_competitor_webdriver import WebDriverSession
from browser_competitor_webdriver_runtime import (
    launch_prepared_runtime,
    prepare_webdriver_runtime,
)
from browser_competitor_webdriver_scenario import (
    request_gc as webdriver_request_gc,
    setup_webdriver_scenario,
    webdriver_native_query,
    webdriver_virtual_query,
)


def _playwright_query(
    page: Any,
    *,
    mode: str,
    offset: int,
    slice_bytes: int,
) -> tuple[float, dict[str, Any]]:
    if mode == "virtualized":
        detail = page.evaluate(
            "([start, length]) => window.__renderVirtualSlice(start, length)",
            [offset, slice_bytes],
        )
        if not isinstance(detail, dict):
            raise ScenarioContractInvalid(
                "Playwright virtual query did not return an object"
            )
        milliseconds = float(detail["milliseconds"])
        if milliseconds < 0:
            raise ScenarioContractInvalid(
                "Playwright virtual query returned negative timing"
            )
        return milliseconds, {"byte_offset": offset, **detail}
    if mode == "native-dom":
        fraction = native_fraction(offset)
        milliseconds = float(
            page.evaluate("fraction => window.__scrollNative(fraction)", fraction)
        )
        if milliseconds < 0:
            raise ScenarioContractInvalid(
                "Playwright native query returned negative timing"
            )
        return milliseconds, {
            "scroll_offset_unit": offset,
            "scroll_fraction": fraction,
            "milliseconds": milliseconds,
        }
    raise ValueError(f"unknown mode: {mode}")


def _webdriver_query(
    session: WebDriverSession,
    *,
    mode: str,
    offset: int,
    slice_bytes: int,
) -> tuple[float, dict[str, Any]]:
    if mode == "virtualized":
        detail = webdriver_virtual_query(
            session,
            offset=offset,
            slice_bytes=slice_bytes,
        )
        milliseconds = float(detail["milliseconds"])
        if milliseconds < 0:
            raise ScenarioContractInvalid(
                "WebDriver virtual query returned negative timing"
            )
        return milliseconds, {"byte_offset": offset, **detail}
    if mode == "native-dom":
        fraction = native_fraction(offset)
        milliseconds = float(webdriver_native_query(session, fraction=fraction))
        if milliseconds < 0:
            raise ScenarioContractInvalid(
                "WebDriver native query returned negative timing"
            )
        return milliseconds, {
            "scroll_offset_unit": offset,
            "scroll_fraction": fraction,
            "milliseconds": milliseconds,
        }
    raise ValueError(f"unknown mode: {mode}")


def _playwright_case(
    competitor: str,
    mode: str,
    payload_bytes: int,
    query_count: int,
    warmup_query_count: int,
    slice_bytes: int,
    case_timeout_seconds: int,
    evidence_kwargs: dict[str, str],
) -> dict[str, object]:
    from playwright.sync_api import sync_playwright

    query_plan = plan_query_offsets(
        mode=mode,
        payload_bytes=payload_bytes,
        virtual_slice_bytes=slice_bytes,
        query_count=query_count,
        warmup_query_count=warmup_query_count,
    )
    browser = None
    context = None
    monitor: BrowserProcessScopeMonitor | None = None
    try:
        root_pid = os.getpid()
        with sync_playwright() as playwright:
            baseline = descendant_identities(root_pid)
            monitor = BrowserProcessScopeMonitor(root_pid, baseline)
            monitor.start()
            setup_started = time.perf_counter()
            try:
                spec, browser, launch_plan = launch_browser(playwright, competitor)
            except BaseException as exc:
                status, reason = launch_failure_status(exc)
                return failure_case_record(
                    competitor,
                    mode,
                    payload_bytes,
                    status,
                    reason,
                    traceback=traceback.format_exc(),
                )

            identity = playwright_runtime_identity(spec, launch_plan, browser.version)
            context = browser.new_context(
                viewport={"width": VIEWPORT_WIDTH, "height": VIEWPORT_HEIGHT}
            )
            page = context.new_page()
            viewport_receipt = _playwright_setup_page(page)

            scenario_setup_started = time.perf_counter()
            if mode == "virtualized":
                setup_metrics = _playwright_setup_virtualized(page, payload_bytes)
            elif mode == "native-dom":
                setup_metrics = _playwright_setup_native(page, payload_bytes)
            else:
                raise ValueError(f"unknown mode: {mode}")
            scenario_setup_wall_seconds = time.perf_counter() - scenario_setup_started

            page.evaluate("() => { if (globalThis.gc) globalThis.gc(); }")
            time.sleep(0.25)
            warmup_details: list[dict[str, Any]] = []
            for offset in query_plan.warmup_offsets:
                _, detail = _playwright_query(
                    page,
                    mode=mode,
                    offset=offset,
                    slice_bytes=slice_bytes,
                )
                warmup_details.append(detail)

            setup_snapshot = monitor.snapshot()
            setup_to_ready_seconds = time.perf_counter() - setup_started

            query_ms: list[float] = []
            query_details: list[dict[str, Any]] = []
            for offset in query_plan.measured_offsets:
                milliseconds, detail = _playwright_query(
                    page,
                    mode=mode,
                    offset=offset,
                    slice_bytes=slice_bytes,
                )
                query_ms.append(milliseconds)
                query_details.append(detail)

            query_snapshot = monitor.snapshot()
            monitor.stop()
            memory = process_scope_metrics(monitor, setup_snapshot, query_snapshot)
            if memory["memory_metric_status"] != "valid":
                return failure_case_record(
                    competitor,
                    mode,
                    payload_bytes,
                    "invalid",
                    str(memory["memory_metric_reason"]),
                    **memory,
                    warmup_query_details=warmup_details,
                    query_details=query_details,
                )

            record: dict[str, object] = {
                **terminal_record(
                    spec,
                    "success",
                    runtime_identity=identity,
                    **evidence_kwargs,
                ),
                "browser": competitor,
                "browser_version": browser.version,
                "adapter_runtime": "playwright",
                "mode": mode,
                "payload_bytes": payload_bytes,
                "query_count": query_count,
                "warmup_query_count": warmup_query_count,
                "slice_bytes": slice_bytes if mode == "virtualized" else None,
                "case_timeout_seconds": case_timeout_seconds,
                "viewport_receipt": viewport_receipt,
                "scenario_setup_wall_seconds": scenario_setup_wall_seconds,
                "normalized_setup_to_ready_seconds": setup_to_ready_seconds,
                "setup_metrics": setup_metrics,
                **_query_summary(query_ms),
                **memory,
                "warmup_query_details": warmup_details,
                "query_details": query_details,
            }
            return attach_browser_normalized_evidence(
                record,
                setup_to_ready_seconds=setup_to_ready_seconds,
                query_samples_ms=query_ms,
                warmup_query_count=warmup_query_count,
                memory_metrics=memory,
            )
    except (ProcessScopeUnavailable, ScenarioContractInvalid, BrowserNormalizedLifecycleInvalid) as exc:
        return failure_case_record(
            competitor,
            mode,
            payload_bytes,
            "invalid",
            f"{type(exc).__name__}: {exc}",
            traceback=traceback.format_exc(),
        )
    except BaseException as exc:
        return failure_case_record(
            competitor,
            mode,
            payload_bytes,
            "error",
            f"{type(exc).__name__}: {exc}",
            traceback=traceback.format_exc(),
        )
    finally:
        if monitor is not None:
            try:
                monitor.stop()
            except BaseException:
                pass
        if context is not None:
            try:
                context.close()
            except BaseException:
                pass
        if browser is not None:
            try:
                browser.close()
            except BaseException:
                pass


def _webdriver_case(
    competitor: str,
    mode: str,
    payload_bytes: int,
    query_count: int,
    warmup_query_count: int,
    slice_bytes: int,
    case_timeout_seconds: int,
    evidence_kwargs: dict[str, str],
) -> dict[str, object]:
    query_plan = plan_query_offsets(
        mode=mode,
        payload_bytes=payload_bytes,
        virtual_slice_bytes=slice_bytes,
        query_count=query_count,
        warmup_query_count=warmup_query_count,
    )
    monitor: BrowserProcessScopeMonitor | None = None
    engine = None
    session: WebDriverSession | None = None
    try:
        spec = get_spec(competitor)
        prepared = prepare_webdriver_runtime(competitor)
        root_pid = os.getpid()
        baseline = descendant_identities(root_pid)
        monitor = BrowserProcessScopeMonitor(root_pid, baseline)
        monitor.start()
        setup_started = time.perf_counter()
        engine = launch_prepared_runtime(
            prepared,
            startup_timeout_seconds=min(
                20.0,
                max(1.0, case_timeout_seconds / 3.0),
            ),
        )
        session = WebDriverSession.create(
            prepared.webdriver_url,
            always_match={"pageLoadStrategy": "normal"},
            timeout_seconds=float(case_timeout_seconds),
        )
        timeout_ms = int(case_timeout_seconds * 1000)
        session.set_timeouts(script_ms=timeout_ms, page_load_ms=timeout_ms)

        scenario_setup_started = time.perf_counter()
        setup_metrics = setup_webdriver_scenario(
            session,
            mode=mode,
            payload_bytes=payload_bytes,
        )
        scenario_setup_wall_seconds = time.perf_counter() - scenario_setup_started
        webdriver_request_gc(session)
        time.sleep(0.25)

        warmup_details: list[dict[str, Any]] = []
        for offset in query_plan.warmup_offsets:
            _, detail = _webdriver_query(
                session,
                mode=mode,
                offset=offset,
                slice_bytes=slice_bytes,
            )
            warmup_details.append(detail)

        setup_snapshot = monitor.snapshot()
        setup_to_ready_seconds = time.perf_counter() - setup_started

        query_ms: list[float] = []
        query_details: list[dict[str, Any]] = []
        for offset in query_plan.measured_offsets:
            milliseconds, detail = _webdriver_query(
                session,
                mode=mode,
                offset=offset,
                slice_bytes=slice_bytes,
            )
            query_ms.append(milliseconds)
            query_details.append(detail)

        query_snapshot = monitor.snapshot()
        monitor.stop()
        memory = process_scope_metrics(monitor, setup_snapshot, query_snapshot)
        if memory["memory_metric_status"] != "valid":
            return failure_case_record(
                competitor,
                mode,
                payload_bytes,
                "invalid",
                str(memory["memory_metric_reason"]),
                **memory,
                warmup_query_details=warmup_details,
                query_details=query_details,
            )

        browser_name = session.capabilities.get("browserName")
        browser_version = session.capabilities.get("browserVersion")
        record: dict[str, object] = {
            **terminal_record(
                spec,
                "success",
                runtime_identity=prepared.runtime_identity,
                **evidence_kwargs,
            ),
            "browser": competitor,
            "browser_version": browser_version if isinstance(browser_version, str) else None,
            "adapter_runtime": "webdriver",
            "mode": mode,
            "payload_bytes": payload_bytes,
            "query_count": query_count,
            "warmup_query_count": warmup_query_count,
            "slice_bytes": slice_bytes if mode == "virtualized" else None,
            "case_timeout_seconds": case_timeout_seconds,
            "runtime_identity_source": prepared.identity_source,
            "webdriver_status_receipt": engine.status_receipt,
            "webdriver_capabilities": dict(session.capabilities),
            "webdriver_browser_name_receipt": browser_name,
            "scenario_setup_wall_seconds": scenario_setup_wall_seconds,
            "normalized_setup_to_ready_seconds": setup_to_ready_seconds,
            "setup_metrics": setup_metrics,
            **_query_summary(query_ms),
            **memory,
            "warmup_query_details": warmup_details,
            "query_details": query_details,
        }
        return attach_browser_normalized_evidence(
            record,
            setup_to_ready_seconds=setup_to_ready_seconds,
            query_samples_ms=query_ms,
            warmup_query_count=warmup_query_count,
            memory_metrics=memory,
        )
    except BrowserNormalizedLifecycleInvalid as exc:
        return failure_case_record(
            competitor,
            mode,
            payload_bytes,
            "invalid",
            f"{type(exc).__name__}: {exc}",
            traceback=traceback.format_exc(),
        )
    except BaseException as exc:
        status, reason = webdriver_failure_status(exc)
        return failure_case_record(
            competitor,
            mode,
            payload_bytes,
            status,
            reason,
            traceback=traceback.format_exc(),
        )
    finally:
        if monitor is not None:
            try:
                monitor.stop()
            except BaseException:
                pass
        if session is not None:
            try:
                session.close()
            except BaseException:
                pass
        if engine is not None:
            try:
                engine.close()
            except BaseException:
                pass


def execute_normalized_case(
    *,
    adapter: str,
    competitor: str,
    mode: str,
    payload_bytes: int,
    query_count: int,
    warmup_query_count: int,
    slice_bytes: int,
    case_timeout_seconds: int,
    evidence_kwargs: dict[str, str],
) -> dict[str, object]:
    if adapter == "playwright":
        return _playwright_case(
            competitor,
            mode,
            payload_bytes,
            query_count,
            warmup_query_count,
            slice_bytes,
            case_timeout_seconds,
            evidence_kwargs,
        )
    if adapter == "webdriver":
        return _webdriver_case(
            competitor,
            mode,
            payload_bytes,
            query_count,
            warmup_query_count,
            slice_bytes,
            case_timeout_seconds,
            evidence_kwargs,
        )
    return failure_case_record(
        competitor,
        mode,
        payload_bytes,
        "unsupported",
        f"adapter {adapter} has no normalized case executor",
    )


def normalized_worker_entry(
    adapter: str,
    competitor: str,
    mode: str,
    payload_bytes: int,
    query_count: int,
    warmup_query_count: int,
    slice_bytes: int,
    case_timeout_seconds: int,
    evidence_kwargs: dict[str, str],
    output_queue: Any,
) -> None:
    try:
        result = execute_normalized_case(
            adapter=adapter,
            competitor=competitor,
            mode=mode,
            payload_bytes=payload_bytes,
            query_count=query_count,
            warmup_query_count=warmup_query_count,
            slice_bytes=slice_bytes,
            case_timeout_seconds=case_timeout_seconds,
            evidence_kwargs=evidence_kwargs,
        )
    except BaseException as exc:
        result = failure_case_record(
            competitor,
            mode,
            payload_bytes,
            "error",
            f"{type(exc).__name__}: {exc}",
            traceback=traceback.format_exc(),
        )
    output_queue.put(result)
