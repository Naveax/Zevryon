#!/usr/bin/env python3
from __future__ import annotations

from browser_competitor_scenario_contract import (
    CONTENT_WIDTH,
    CORPUS_CHUNK_BYTES,
    MEMORY_ACCOUNTING_POLICY,
    OFFSET_GENERATOR_POLICY,
    PAYLOAD_GENERATOR_POLICY,
    PAYLOAD_PATTERN_TEXT,
    SCENARIO_HTML,
    SCRIPT_COMPLETION_POLICY,
    SYNTHETIC_PATTERN,
    VIEWPORT_HEIGHT,
    VIEWPORT_POLICY,
    VIEWPORT_WIDTH,
    WARMUP_POLICY,
    ScenarioContractInvalid,
    deterministic_offsets,
    scenario_semantics,
    timing_boundary,
    validate_exact_viewport,
)


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
    require(VIEWPORT_WIDTH == 800 and VIEWPORT_HEIGHT == 720, "canonical viewport drifted")
    require(CONTENT_WIDTH == 776, "canonical content width drifted")
    require(CORPUS_CHUNK_BYTES == 1024 * 1024, "corpus chunk size drifted")
    require(PAYLOAD_PATTERN_TEXT.encode("utf-8") == SYNTHETIC_PATTERN, "payload pattern identity drifted")
    require(
        '#scroller { width: 800px; height: 720px;' in SCENARIO_HTML,
        "scenario HTML viewport geometry drifted",
    )
    require('width: 776px;' in SCENARIO_HTML, "scenario HTML content width drifted")
    require('font: 16px/18px monospace;' in SCENARIO_HTML, "scenario font geometry drifted")

    receipt = validate_exact_viewport({"width": 800, "height": 720})
    require(receipt.as_dict() == {"width": 800, "height": 720}, "viewport receipt drifted")
    require_raises(
        ScenarioContractInvalid,
        lambda: validate_exact_viewport({"width": 799, "height": 720}),
        "mismatched viewport was admitted",
    )
    require_raises(
        ScenarioContractInvalid,
        lambda: validate_exact_viewport({"width": True, "height": 720}),
        "boolean viewport width was admitted",
    )

    first = deterministic_offsets(64 * 1024 * 1024, 128 * 1024, 5)
    second = deterministic_offsets(64 * 1024 * 1024, 128 * 1024, 5)
    require(first == second, "deterministic offset sequence drifted")
    require(len(first) == 5, "offset count drifted")
    require(all(0 <= value <= 64 * 1024 * 1024 - 128 * 1024 for value in first), "offset escaped payload bounds")
    require(
        deterministic_offsets(1, 2, 3) == [0, 0, 0],
        "oversized slice offset policy drifted",
    )
    require_raises(
        ValueError,
        lambda: deterministic_offsets(0, 1, 1),
        "non-positive payload was accepted by offset generator",
    )

    virtual = scenario_semantics("virtualized")
    native = scenario_semantics("native-dom")
    require(virtual["viewport_policy"] == VIEWPORT_POLICY, "viewport policy omitted")
    require(virtual["warmup_policy"] == WARMUP_POLICY, "warmup policy omitted")
    require(virtual["memory_accounting_policy"] == MEMORY_ACCOUNTING_POLICY, "memory policy omitted")
    require(virtual["script_completion_policy"] == SCRIPT_COMPLETION_POLICY, "script completion policy omitted")
    require(virtual["payload_generator"] == PAYLOAD_GENERATOR_POLICY, "payload generator omitted")
    require(virtual["offset_generator"] == OFFSET_GENERATOR_POLICY, "offset generator omitted")
    require(virtual["timing_boundary"] == "blob-slice-text-layout-double-raf", "virtual timing boundary drifted")
    require(native["timing_boundary"] == "scroll-layout-double-raf", "native timing boundary drifted")
    require(virtual != native, "mode semantics collapsed")
    require(timing_boundary("virtualized") == virtual["timing_boundary"], "timing helper drifted")
    require_raises(ValueError, lambda: timing_boundary("unknown"), "unknown timing mode was accepted")

    print("Zevryon competitor scenario-contract tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
