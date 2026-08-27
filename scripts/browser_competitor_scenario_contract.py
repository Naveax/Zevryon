#!/usr/bin/env python3
from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Mapping

from m7_synthetic_corpus import (
    CORPUS_CHUNK_BYTES,
    PAYLOAD_PATTERN_TEXT,
    SYNTHETIC_PATTERN,
)


VIEWPORT_WIDTH = 800
VIEWPORT_HEIGHT = 720
CONTENT_HORIZONTAL_PADDING = 12
CONTENT_WIDTH = VIEWPORT_WIDTH - (2 * CONTENT_HORIZONTAL_PADDING)
MEMORY_ACCOUNTING_POLICY = (
    "dedicated-worker-post-control-baseline-descendants:"
    "pid-plus-create-time:pss-linux-rss-fallback-v2"
)
VIEWPORT_POLICY = "exact-inner-css-viewport-800x720-v1"
WARMUP_POLICY = "setup-gc-250ms"
SCRIPT_COMPLETION_POLICY = "double-raf-playwright-promise-or-w3c-async-callback-v1"
PAYLOAD_GENERATOR_POLICY = "zevryon.m7.synthetic-unicode-blob.v1"
OFFSET_GENERATOR_POLICY = "lcg-0x243f6a88-v1"

SCENARIO_HTML = f"""<!doctype html>
<meta charset="utf-8">
<style>
html, body {{ margin: 0; padding: 0; background: white; }}
#scroller {{ width: {VIEWPORT_WIDTH}px; height: {VIEWPORT_HEIGHT}px; overflow: auto; contain: strict; }}
#content {{
  box-sizing: border-box;
  width: {CONTENT_WIDTH}px;
  margin: 0;
  padding: 6px {CONTENT_HORIZONTAL_PADDING}px;
  font: 16px/18px monospace;
  white-space: pre-wrap;
  overflow-wrap: anywhere;
}}
</style>
<div id="scroller"><pre id="content"></pre></div>
"""


class ScenarioContractInvalid(RuntimeError):
    pass


@dataclass(frozen=True)
class ViewportReceipt:
    width: int
    height: int

    def as_dict(self) -> dict[str, int]:
        return {"width": self.width, "height": self.height}


def validate_exact_viewport(value: Mapping[str, Any]) -> ViewportReceipt:
    width = value.get("width")
    height = value.get("height")
    if (
        not isinstance(width, int)
        or isinstance(width, bool)
        or not isinstance(height, int)
        or isinstance(height, bool)
    ):
        raise ScenarioContractInvalid("inner viewport receipt must contain integer width/height")
    if (width, height) != (VIEWPORT_WIDTH, VIEWPORT_HEIGHT):
        raise ScenarioContractInvalid(
            f"inner viewport mismatch: expected {VIEWPORT_WIDTH}x{VIEWPORT_HEIGHT}, "
            f"got {width}x{height}"
        )
    return ViewportReceipt(width=width, height=height)


def deterministic_offsets(payload_bytes: int, slice_bytes: int, count: int) -> list[int]:
    if payload_bytes <= 0 or slice_bytes <= 0 or count <= 0:
        raise ValueError("offset generator arguments must be positive")
    maximum = max(0, payload_bytes - slice_bytes)
    state = 0x243F6A88
    output: list[int] = []
    for _ in range(count):
        state = (state * 1664525 + 1013904223) & 0xFFFFFFFF
        output.append((state * maximum) // 0xFFFFFFFF if maximum else 0)
    return output


def timing_boundary(mode: str) -> str:
    if mode == "virtualized":
        return "blob-slice-text-layout-double-raf"
    if mode == "native-dom":
        return "scroll-layout-double-raf"
    raise ValueError(f"unknown benchmark mode: {mode}")


def scenario_semantics(mode: str) -> dict[str, object]:
    return {
        "viewport": {"width": VIEWPORT_WIDTH, "height": VIEWPORT_HEIGHT},
        "viewport_policy": VIEWPORT_POLICY,
        "payload_generator": PAYLOAD_GENERATOR_POLICY,
        "offset_generator": OFFSET_GENERATOR_POLICY,
        "warmup_policy": WARMUP_POLICY,
        "memory_accounting_policy": MEMORY_ACCOUNTING_POLICY,
        "script_completion_policy": SCRIPT_COMPLETION_POLICY,
        "timing_boundary": timing_boundary(mode),
    }
