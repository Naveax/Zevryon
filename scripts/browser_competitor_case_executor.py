#!/usr/bin/env python3
from __future__ import annotations

import json
import os
import statistics
import time
import traceback
from typing import Any

from browser_competitor_playwright import (
    launch_browser,
    launch_failure_status,
    runtime_identity as playwright_runtime_identity,
)
from browser_competitor_process_scope import (
    BrowserProcessScopeMonitor,
    ProcessScopeSnapshot,
    ProcessScopeUnavailable,
    descendant_identities,
)
from browser_competitor_registry import get_spec, terminal_record
from browser_competitor_scenario_contract import (
    CORPUS_CHUNK_BYTES,
    PAYLOAD_PATTERN_TEXT,
    SCENARIO_HTML,
    SYNTHETIC_PATTERN,
    VIEWPORT_HEIGHT,
    VIEWPORT_WIDTH,
    ScenarioContractInvalid,
    deterministic_offsets,
    validate_exact_viewport,
)
from browser_competitor_webdriver import (
    WebDriverProtocolError,
    WebDriverSession,
    WebDriverTransportError,
)
from browser_competitor_webdriver_runtime import (
    WebDriverRuntimeInvalid,
    WebDriverRuntimeLaunchError,
    WebDriverRuntimeUnavailable,
    launch_prepared_runtime,
    prepare_webdriver_runtime,
)
from browser_competitor_webdriver_scenario import (
    WebDriverScenarioInvalid,
    request_gc as webdriver_request_gc,
    setup_webdriver_scenario,
    webdriver_native_query,
    webdriver_virtual_query,
)


_UNSUPPORTED_WEBDRIVER_MARKERS = (
    "unknown command",
    "unsupported operation",
    "unsupported command",
)

_PATTERN_JS = json.dumps(PAYLOAD_PATTERN_TEXT, ensure_ascii=False)


def percentile(values: list[float], value: float) -> float:
    ordered = sorted(values)
    if not ordered:
        raise ValueError("percentile requires samples")
    position = (len(ordered) - 1) * value / 100.0
    lower = int(position)
    upper = min(lower + 1, len(ordered) - 1)
    fraction = position - lower
    return ordered[lower] * (1.0 - fraction) + ordered[upper] * fraction


def failure_case_record(
    competitor: str,
    mode: str,
    payload_bytes: int,
    status: str,
    reason: str,
    **extra: object,
) -> dict[str, object]:
    spec = get_spec(competitor)
    record = terminal_record(spec, status, reason=reason)
    return {
        **record,
        "browser": competitor,
        "mode": mode,
        "payload_bytes": payload_bytes,
        **extra,
    }


def webdriver_failure_status(exc: BaseException) -> tuple[str, str]:
    reason = f"{type(exc).__name__}: {exc}".strip()
    if isinstance(exc, WebDriverRuntimeUnavailable):
        return "unavailable", reason
    if isinstance(exc, (WebDriverRuntimeInvalid, WebDriverScenarioInvalid, ScenarioContractInvalid, ProcessScopeUnavailable)):
        return "invalid", reason
    if isinstance(exc, WebDriverProtocolError):
        lowered = reason.lower()
        if any(marker in lowered for marker in _UNSUPPORTED_WEBDRIVER_MARKERS):
            return "unsupported", reason
        return "error", reason
    if isinstance(exc, (WebDriverRuntimeLaunchError, WebDriverTransportError)):
        return "error", reason
    return "error", reason


def process_scope_metrics(
    monitor: BrowserProcessScopeMonitor,
    setup_snapshot: ProcessScopeSnapshot,
    query_snapshot: ProcessScopeSnapshot,
) -> dict[str, object]:
    valid = monitor.valid and setup_snapshot.valid and query_snapshot.valid
    reason = None
    if not valid:
        missing: list[str] = []
        if not monitor.valid:
            missing.append("no browser process identity was observed")
        if not setup_snapshot.valid:
            missing.append("browser process scope was empty after setup")
        if not query_snapshot.valid:
            missing.append("browser process scope was empty after queries")
        reason = "; ".join(missing)
    return {
        "memory_metric_status": "valid" if valid else "invalid",
        "memory_metric_reason": reason,
        "browser_scope_resident_mb_after_setup": setup_snapshot.resident_bytes / 1_000_000,
        "browser_scope_resident_mb_after_queries": query_snapshot.resident_bytes / 1_000_000,
        "browser_scope_peak_mb": monitor.peak_bytes / 1_000_000,
        "browser_scope_setup_receipts": setup_snapshot.receipts,
        "browser_scope_query_receipts": query_snapshot.receipts,
        "browser_scope_observed_receipts": monitor.observed_receipts,
    }


def _query_summary(query_ms: list[float]) -> dict[str, float]:
    return {
        "query_milliseconds_p50": percentile(query_ms, 50.0),
        "query_milliseconds_p95": percentile(query_ms, 95.0),
        "query_milliseconds_p99": percentile(query_ms, 99.0),
        "query_milliseconds_max": max(query_ms),
        "query_milliseconds_mean": statistics.fmean(query_ms),
    }


def _playwright_inner_viewport(page: Any) -> dict[str, int]:
    receipt = page.evaluate(
        "() => ({ width: Number(window.innerWidth), height: Number(window.innerHeight) })"
    )
    if not isinstance(receipt, dict):
        raise ScenarioContractInvalid("Playwright inner viewport receipt is not an object")
    normalized = {
        "width": receipt.get("width"),
        "height": receipt.get("height"),
    }
    return validate_exact_viewport(normalized).as_dict()


def _playwright_setup_page(page: Any) -> dict[str, int]:
    page.set_content(SCENARIO_HTML)
    return _playwright_inner_viewport(page)


def _playwright_create_blob(page: Any, payload_bytes: int) -> dict[str, Any]:
    result = page.evaluate(
        f"""
async (payloadBytes) => {{
  const encoder = new TextEncoder();
  const pattern = encoder.encode({_PATTERN_JS});
  const chunkBytes = {CORPUS_CHUNK_BYTES};
  const chunk = new Uint8Array(chunkBytes);
  for (let index = 0; index < pattern.length; ++index) chunk[index] = pattern[index];
  let filled = pattern.length;
  while (filled < chunk.length) {{
    const copy = Math.min(filled, chunk.length - filled);
    chunk.set(chunk.subarray(0, copy), filled);
    filled += copy;
  }}
  const fullChunks = Math.floor(payloadBytes / chunkBytes);
  const remainder = payloadBytes % chunkBytes;
  const parts = Array(fullChunks).fill(chunk);
  if (remainder) parts.push(chunk.subarray(0, remainder));
  window.__payloadBlob = new Blob(parts, {{ type: 'text/plain;charset=utf-8' }});
  window.__payloadBytes = payloadBytes;
  return {{ blob_bytes: window.__payloadBlob.size, pattern_bytes: pattern.length }};
}}
""",
        payload_bytes,
    )
    if not isinstance(result, dict):
        raise ScenarioContractInvalid("Playwright blob setup did not return an object")
    if result.get("blob_bytes") != payload_bytes:
        raise ScenarioContractInvalid(
            f"Playwright blob byte count mismatch: expected {payload_bytes}, got {result.get('blob_bytes')}"
        )
    if result.get("pattern_bytes") != len(SYNTHETIC_PATTERN):
        raise ScenarioContractInvalid("Playwright payload pattern byte count drifted")
    return dict(result)


def _playwright_setup_virtualized(page: Any, payload_bytes: int) -> dict[str, Any]:
    blob = _playwright_create_blob(page, payload_bytes)
    page.evaluate(
        """
() => {
  document.getElementById('content').textContent = '';
  window.__renderVirtualSlice = async (start, length) => {
    const started = performance.now();
    const text = await window.__payloadBlob.slice(start, start + length).text();
    const content = document.getElementById('content');
    content.textContent = text;
    void content.offsetHeight;
    await new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)));
    return {
      milliseconds: performance.now() - started,
      rendered_utf16_units: text.length,
      rendered_height: content.offsetHeight,
    };
  };
}
"""
    )
    return blob


def _playwright_setup_native(page: Any, payload_bytes: int) -> dict[str, Any]:
    blob = _playwright_create_blob(page, payload_bytes)
    native = page.evaluate(
        """
async () => {
  const started = performance.now();
  const text = await window.__payloadBlob.text();
  const content = document.getElementById('content');
  content.textContent = text;
  const scrollHeight = document.getElementById('scroller').scrollHeight;
  await new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)));
  window.__nativeText = text;
  window.__scrollNative = async fraction => {
    const scroller = document.getElementById('scroller');
    const began = performance.now();
    scroller.scrollTop = Math.floor(Math.max(0, scroller.scrollHeight - scroller.clientHeight) * fraction);
    void content.offsetHeight;
    await new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)));
    return performance.now() - began;
  };
  return {
    setup_milliseconds: performance.now() - started,
    decoded_utf16_units: text.length,
    scroll_height: scrollHeight,
  };
}
"""
    )
    if not isinstance(native, dict):
        raise ScenarioContractInvalid("Playwright native setup did not return an object")
    return {**blob, **native}


def _playwright_case(
    competitor: str,
    mode: str,
    payload_bytes: int,
    query_count: int,
    slice_bytes: int,
    case_timeout_seconds: int,
    evidence_kwargs: dict[str, str],
) -> dict[str, object]:
    from playwright.sync_api import sync_playwright

    browser = None
    context = None
    monitor: BrowserProcessScopeMonitor | None = None
    try:
        root_pid = os.getpid()
        with sync_playwright() as playwright:
            baseline = descendant_identities(root_pid)
            monitor = BrowserProcessScopeMonitor(root_pid, baseline)
            monitor.start()
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

            setup_started = time.perf_counter()
            if mode == "virtualized":
                setup_metrics = _playwright_setup_virtualized(page, payload_bytes)
            elif mode == "native-dom":
                setup_metrics = _playwright_setup_native(page, payload_bytes)
            else:
                raise ValueError(f"unknown mode: {mode}")
            setup_wall_seconds = time.perf_counter() - setup_started
            page.evaluate("() => { if (globalThis.gc) globalThis.gc(); }")
            time.sleep(0.25)
            setup_snapshot = monitor.snapshot()

            query_ms: list[float] = []
            query_details: list[dict[str, Any]] = []
            if mode == "virtualized":
                offsets = deterministic_offsets(payload_bytes, slice_bytes, query_count)
                for offset in offsets:
                    detail = page.evaluate(
                        "([start, length]) => window.__renderVirtualSlice(start, length)",
                        [offset, slice_bytes],
                    )
                    if not isinstance(detail, dict):
                        raise ScenarioContractInvalid("Playwright virtual query did not return an object")
                    milliseconds = float(detail["milliseconds"])
                    if milliseconds < 0:
                        raise ScenarioContractInvalid("Playwright virtual query returned negative timing")
                    query_ms.append(milliseconds)
                    query_details.append({"byte_offset": offset, **detail})
            else:
                offsets = deterministic_offsets(1_000_000, 1, query_count)
                for offset in offsets:
                    fraction = offset / 1_000_000.0
                    milliseconds = float(
                        page.evaluate("fraction => window.__scrollNative(fraction)", fraction)
                    )
                    if milliseconds < 0:
                        raise ScenarioContractInvalid("Playwright native query returned negative timing")
                    query_ms.append(milliseconds)
                    query_details.append(
                        {"scroll_fraction": fraction, "milliseconds": milliseconds}
                    )

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
                    query_details=query_details,
                )

            return {
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
                "slice_bytes": slice_bytes if mode == "virtualized" else None,
                "case_timeout_seconds": case_timeout_seconds,
                "viewport_receipt": viewport_receipt,
                "setup_wall_seconds": setup_wall_seconds,
                "setup_metrics": setup_metrics,
                **_query_summary(query_ms),
                **memory,
                "query_details": query_details,
            }
    except ProcessScopeUnavailable as exc:
        return failure_case_record(
            competitor,
            mode,
            payload_bytes,
            "invalid",
            str(exc),
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
    slice_bytes: int,
    case_timeout_seconds: int,
    evidence_kwargs: dict[str, str],
) -> dict[str, object]:
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
        engine = launch_prepared_runtime(
            prepared,
            startup_timeout_seconds=min(20.0, max(1.0, case_timeout_seconds / 3.0)),
        )
        session = WebDriverSession.create(
            prepared.webdriver_url,
            always_match={"pageLoadStrategy": "normal"},
            timeout_seconds=float(case_timeout_seconds),
        )
        timeout_ms = int(case_timeout_seconds * 1000)
        session.set_timeouts(script_ms=timeout_ms, page_load_ms=timeout_ms)

        setup_started = time.perf_counter()
        setup_metrics = setup_webdriver_scenario(
            session,
            mode=mode,
            payload_bytes=payload_bytes,
        )
        setup_wall_seconds = time.perf_counter() - setup_started
        webdriver_request_gc(session)
        time.sleep(0.25)
        setup_snapshot = monitor.snapshot()

        query_ms: list[float] = []
        query_details: list[dict[str, Any]] = []
        if mode == "virtualized":
            offsets = deterministic_offsets(payload_bytes, slice_bytes, query_count)
            for offset in offsets:
                detail = webdriver_virtual_query(
                    session,
                    offset=offset,
                    slice_bytes=slice_bytes,
                )
                milliseconds = float(detail["milliseconds"])
                query_ms.append(milliseconds)
                query_details.append({"byte_offset": offset, **detail})
        elif mode == "native-dom":
            offsets = deterministic_offsets(1_000_000, 1, query_count)
            for offset in offsets:
                fraction = offset / 1_000_000.0
                milliseconds = webdriver_native_query(session, fraction=fraction)
                query_ms.append(milliseconds)
                query_details.append(
                    {"scroll_fraction": fraction, "milliseconds": milliseconds}
                )
        else:
            raise ValueError(f"unknown mode: {mode}")

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
                query_details=query_details,
            )

        browser_name = session.capabilities.get("browserName")
        browser_version = session.capabilities.get("browserVersion")
        return {
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
            "slice_bytes": slice_bytes if mode == "virtualized" else None,
            "case_timeout_seconds": case_timeout_seconds,
            "runtime_identity_source": prepared.identity_source,
            "webdriver_status_receipt": engine.status_receipt,
            "webdriver_capabilities": dict(session.capabilities),
            "webdriver_browser_name_receipt": browser_name,
            "setup_wall_seconds": setup_wall_seconds,
            "setup_metrics": setup_metrics,
            **_query_summary(query_ms),
            **memory,
            "query_details": query_details,
        }
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


def execute_case(
    *,
    adapter: str,
    competitor: str,
    mode: str,
    payload_bytes: int,
    query_count: int,
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
            slice_bytes,
            case_timeout_seconds,
            evidence_kwargs,
        )
    return failure_case_record(
        competitor,
        mode,
        payload_bytes,
        "unsupported",
        f"adapter {adapter} has no case executor",
    )


def worker_entry(
    adapter: str,
    competitor: str,
    mode: str,
    payload_bytes: int,
    query_count: int,
    slice_bytes: int,
    case_timeout_seconds: int,
    evidence_kwargs: dict[str, str],
    output_queue: Any,
) -> None:
    try:
        result = execute_case(
            adapter=adapter,
            competitor=competitor,
            mode=mode,
            payload_bytes=payload_bytes,
            query_count=query_count,
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
