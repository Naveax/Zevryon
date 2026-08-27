#!/usr/bin/env python3
from __future__ import annotations

from typing import Any, Mapping

from browser_competitor_scenario_contract import (
    CORPUS_CHUNK_BYTES,
    PAYLOAD_PATTERN_TEXT,
    SCENARIO_HTML,
    SYNTHETIC_PATTERN,
    VIEWPORT_HEIGHT,
    VIEWPORT_WIDTH,
    ScenarioContractInvalid,
    validate_exact_viewport,
)


WEBDRIVER_SCENARIO_POLICY = "w3c-async-callback-double-raf-inner-viewport-v1"

INNER_VIEWPORT_SCRIPT = """
return { width: Number(window.innerWidth), height: Number(window.innerHeight) };
"""

INSTALL_DOCUMENT_SCRIPT = """
document.open();
document.write(arguments[0]);
document.close();
return true;
"""

CREATE_BLOB_SCRIPT = f"""
const payloadBytes = Number(arguments[0]);
const encoder = new TextEncoder();
const pattern = encoder.encode({PAYLOAD_PATTERN_TEXT!r});
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
"""

VIRTUAL_QUERY_ASYNC_SCRIPT = """
const start = Number(arguments[0]);
const length = Number(arguments[1]);
const done = arguments[arguments.length - 1];
(async () => {
  try {
    const started = performance.now();
    const text = await window.__payloadBlob.slice(start, start + length).text();
    const content = document.getElementById('content');
    content.textContent = text;
    void content.offsetHeight;
    await new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)));
    done({
      ok: true,
      value: {
        milliseconds: performance.now() - started,
        rendered_utf16_units: text.length,
        rendered_height: content.offsetHeight,
      },
    });
  } catch (error) {
    done({ ok: false, error: String(error && (error.stack || error.message) || error) });
  }
})();
"""

NATIVE_SETUP_ASYNC_SCRIPT = """
const done = arguments[arguments.length - 1];
(async () => {
  try {
    const started = performance.now();
    const text = await window.__payloadBlob.text();
    const content = document.getElementById('content');
    content.textContent = text;
    const scrollHeight = document.getElementById('scroller').scrollHeight;
    await new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)));
    window.__nativeText = text;
    done({
      ok: true,
      value: {
        setup_milliseconds: performance.now() - started,
        decoded_utf16_units: text.length,
        scroll_height: scrollHeight,
      },
    });
  } catch (error) {
    done({ ok: false, error: String(error && (error.stack || error.message) || error) });
  }
})();
"""

NATIVE_QUERY_ASYNC_SCRIPT = """
const fraction = Number(arguments[0]);
const done = arguments[arguments.length - 1];
try {
  const scroller = document.getElementById('scroller');
  const content = document.getElementById('content');
  const began = performance.now();
  scroller.scrollTop = Math.floor(Math.max(0, scroller.scrollHeight - scroller.clientHeight) * fraction);
  void content.offsetHeight;
  requestAnimationFrame(() => requestAnimationFrame(() => {
    done({ ok: true, value: performance.now() - began });
  }));
} catch (error) {
  done({ ok: false, error: String(error && (error.stack || error.message) || error) });
}
"""

GC_SCRIPT = """
if (globalThis.gc) globalThis.gc();
return null;
"""


class WebDriverScenarioInvalid(RuntimeError):
    pass


def _mapping(value: Any, *, context: str) -> Mapping[str, Any]:
    if not isinstance(value, dict):
        raise WebDriverScenarioInvalid(f"{context} must return an object")
    return value


def _positive_int(value: Any, *, context: str) -> int:
    if not isinstance(value, int) or isinstance(value, bool) or value <= 0:
        raise WebDriverScenarioInvalid(f"{context} must be a positive integer")
    return value


def _unwrap_async(value: Any, *, context: str) -> Any:
    envelope = _mapping(value, context=context)
    if envelope.get("ok") is not True:
        reason = envelope.get("error")
        raise WebDriverScenarioInvalid(
            f"{context} failed: {reason if isinstance(reason, str) and reason else 'unknown JavaScript error'}"
        )
    if "value" not in envelope:
        raise WebDriverScenarioInvalid(f"{context} response lacks value")
    return envelope["value"]


def _inner_viewport(session: Any) -> tuple[int, int]:
    raw = _mapping(
        session.execute_sync(INNER_VIEWPORT_SCRIPT),
        context="inner viewport receipt",
    )
    width = raw.get("width")
    height = raw.get("height")
    if (
        not isinstance(width, int)
        or isinstance(width, bool)
        or not isinstance(height, int)
        or isinstance(height, bool)
        or width <= 0
        or height <= 0
    ):
        raise WebDriverScenarioInvalid("inner viewport receipt must contain positive integers")
    return width, height


def enforce_inner_viewport(
    session: Any,
    *,
    width: int = VIEWPORT_WIDTH,
    height: int = VIEWPORT_HEIGHT,
    max_attempts: int = 4,
) -> dict[str, Any]:
    if width <= 0 or height <= 0 or max_attempts <= 0:
        raise ValueError("viewport dimensions and max_attempts must be positive")
    outer_width = int(width)
    outer_height = int(height)
    attempts: list[dict[str, int]] = []
    for _ in range(max_attempts):
        session.set_window_rect(width=outer_width, height=outer_height)
        inner_width, inner_height = _inner_viewport(session)
        attempts.append(
            {
                "outer_width": outer_width,
                "outer_height": outer_height,
                "inner_width": inner_width,
                "inner_height": inner_height,
            }
        )
        if inner_width == width and inner_height == height:
            if (width, height) == (VIEWPORT_WIDTH, VIEWPORT_HEIGHT):
                try:
                    validate_exact_viewport(
                        {"width": inner_width, "height": inner_height}
                    )
                except ScenarioContractInvalid as exc:
                    raise WebDriverScenarioInvalid(str(exc)) from exc
            return {
                "policy": "calibrate-outer-verify-inner-v1",
                "target_width": width,
                "target_height": height,
                "attempts": attempts,
            }
        outer_width += width - inner_width
        outer_height += height - inner_height
        if outer_width <= 0 or outer_height <= 0:
            break
    raise WebDriverScenarioInvalid(
        f"failed to obtain exact inner viewport {width}x{height}; attempts={attempts}"
    )


def setup_webdriver_scenario(
    session: Any,
    *,
    mode: str,
    payload_bytes: int,
) -> dict[str, Any]:
    if mode not in {"virtualized", "native-dom"}:
        raise ValueError(f"unknown benchmark mode: {mode}")
    if payload_bytes <= 0:
        raise ValueError("payload_bytes must be positive")
    if SYNTHETIC_PATTERN != PAYLOAD_PATTERN_TEXT.encode("utf-8"):
        raise WebDriverScenarioInvalid("shared payload pattern identity drifted")

    session.navigate("about:blank")
    viewport_receipt = enforce_inner_viewport(session)
    session.execute_sync(INSTALL_DOCUMENT_SCRIPT, [SCENARIO_HTML])
    inner_width, inner_height = _inner_viewport(session)
    try:
        validate_exact_viewport({"width": inner_width, "height": inner_height})
    except ScenarioContractInvalid as exc:
        raise WebDriverScenarioInvalid(
            "document installation changed the admitted inner viewport: " + str(exc)
        ) from exc

    blob = _mapping(
        session.execute_sync(CREATE_BLOB_SCRIPT, [payload_bytes]),
        context="blob setup",
    )
    blob_bytes = _positive_int(blob.get("blob_bytes"), context="blob byte count")
    pattern_bytes = _positive_int(blob.get("pattern_bytes"), context="pattern byte count")
    if blob_bytes != payload_bytes:
        raise WebDriverScenarioInvalid(
            f"blob byte count mismatch: expected {payload_bytes}, got {blob_bytes}"
        )
    if pattern_bytes != len(SYNTHETIC_PATTERN):
        raise WebDriverScenarioInvalid(
            f"payload pattern byte count mismatch: expected {len(SYNTHETIC_PATTERN)}, got {pattern_bytes}"
        )

    setup: dict[str, Any] = {
        "blob_bytes": blob_bytes,
        "pattern_bytes": pattern_bytes,
        "viewport": viewport_receipt,
        "scenario_policy": WEBDRIVER_SCENARIO_POLICY,
    }
    if mode == "native-dom":
        native = _mapping(
            _unwrap_async(
                session.execute_async(NATIVE_SETUP_ASYNC_SCRIPT),
                context="native DOM setup",
            ),
            context="native DOM setup value",
        )
        setup.update(native)
    return setup


def webdriver_virtual_query(session: Any, *, offset: int, slice_bytes: int) -> dict[str, Any]:
    if offset < 0 or slice_bytes <= 0:
        raise ValueError("virtual query offset must be non-negative and slice_bytes positive")
    detail = _mapping(
        _unwrap_async(
            session.execute_async(
                VIRTUAL_QUERY_ASYNC_SCRIPT,
                [int(offset), int(slice_bytes)],
            ),
            context="virtualized query",
        ),
        context="virtualized query value",
    )
    milliseconds = detail.get("milliseconds")
    if not isinstance(milliseconds, (int, float)) or isinstance(milliseconds, bool) or milliseconds < 0:
        raise WebDriverScenarioInvalid("virtualized query returned invalid milliseconds")
    return dict(detail)


def webdriver_native_query(session: Any, *, fraction: float) -> float:
    if not isinstance(fraction, (int, float)) or isinstance(fraction, bool) or not 0.0 <= float(fraction) <= 1.0:
        raise ValueError("native query fraction must be in [0, 1]")
    value = _unwrap_async(
        session.execute_async(NATIVE_QUERY_ASYNC_SCRIPT, [float(fraction)]),
        context="native DOM query",
    )
    if not isinstance(value, (int, float)) or isinstance(value, bool) or value < 0:
        raise WebDriverScenarioInvalid("native DOM query returned invalid milliseconds")
    return float(value)


def request_gc(session: Any) -> None:
    session.execute_sync(GC_SCRIPT)
