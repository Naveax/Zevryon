#!/usr/bin/env python3
from __future__ import annotations

import copy

from browser_competitor_normalized_browser_lifecycle import (
    BrowserNormalizedLifecycleInvalid,
    NORMALIZED_MEMORY_SCOPE,
    NORMALIZED_SETUP_BOUNDARY,
    attach_browser_normalized_evidence,
    normalized_incremental_peak_memory_mb,
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


def terminal() -> dict[str, object]:
    return {
        "status": "success",
        "competitor": "chrome",
        "host_platform": "Linux",
        "host_arch": "x86_64",
        "system_fingerprint": "1" * 64,
        "harness_schema": "zevryon.competitor.giant-document.v2",
        "corpus_sha256": "2" * 64,
        "scenario_fingerprint": "3" * 64,
    }


def memory() -> dict[str, object]:
    return {
        "memory_metric_status": "valid",
        "browser_scope_peak_mb": 12.5,
        "browser_scope_resident_mb_after_setup": 9.0,
        "browser_scope_resident_mb_after_queries": 11.0,
    }


def main() -> int:
    require(
        normalized_incremental_peak_memory_mb(memory()) == 12.5,
        "valid process-scope peak drifted",
    )

    attached = attach_browser_normalized_evidence(
        terminal(),
        setup_to_ready_seconds=1.25,
        query_samples_ms=[1.0, 2.0, 3.0, 4.0],
        warmup_query_count=3,
        memory_metrics=memory(),
    )
    evidence = attached["normalized_core_evidence"]
    require(isinstance(evidence, dict), "normalized evidence was not attached")
    require(
        evidence["setup_boundary"] == NORMALIZED_SETUP_BOUNDARY,
        "setup boundary authority drifted",
    )
    require(
        evidence["memory_scope"] == NORMALIZED_MEMORY_SCOPE,
        "memory scope authority drifted",
    )
    require(evidence["warmup_query_count"] == 3, "warmup count drifted")
    require(evidence["query_samples_ms"] == [1.0, 2.0, 3.0, 4.0], "raw samples drifted")
    metrics = evidence["core_metrics"]
    require(metrics["setup_to_ready_seconds"] == 1.25, "normalized setup drifted")
    require(metrics["query_milliseconds_p50"] == 2.5, "normalized p50 drifted")
    require(metrics["incremental_peak_memory_mb"] == 12.5, "normalized memory drifted")

    failed_terminal = terminal()
    failed_terminal["status"] = "error"
    require_raises(
        BrowserNormalizedLifecycleInvalid,
        lambda: attach_browser_normalized_evidence(
            failed_terminal,
            setup_to_ready_seconds=1.0,
            query_samples_ms=[1.0],
            warmup_query_count=0,
            memory_metrics=memory(),
        ),
        "failed terminal accepted normalized evidence",
    )

    missing_identity = terminal()
    del missing_identity["scenario_fingerprint"]
    require_raises(
        BrowserNormalizedLifecycleInvalid,
        lambda: attach_browser_normalized_evidence(
            missing_identity,
            setup_to_ready_seconds=1.0,
            query_samples_ms=[1.0],
            warmup_query_count=0,
            memory_metrics=memory(),
        ),
        "terminal with missing identity accepted normalized evidence",
    )

    invalid_memory = memory()
    invalid_memory["memory_metric_status"] = "invalid"
    require_raises(
        BrowserNormalizedLifecycleInvalid,
        lambda: normalized_incremental_peak_memory_mb(invalid_memory),
        "invalid process scope accepted normalized memory",
    )

    zero_memory = memory()
    zero_memory["browser_scope_peak_mb"] = 0.0
    require_raises(
        BrowserNormalizedLifecycleInvalid,
        lambda: normalized_incremental_peak_memory_mb(zero_memory),
        "zero process-scope peak accepted normalized memory",
    )

    boolean_memory = memory()
    boolean_memory["browser_scope_peak_mb"] = True
    require_raises(
        BrowserNormalizedLifecycleInvalid,
        lambda: normalized_incremental_peak_memory_mb(boolean_memory),
        "boolean process-scope peak accepted normalized memory",
    )

    bad_setup = terminal()
    require_raises(
        BrowserNormalizedLifecycleInvalid,
        lambda: attach_browser_normalized_evidence(
            bad_setup,
            setup_to_ready_seconds=-0.1,
            query_samples_ms=[1.0],
            warmup_query_count=0,
            memory_metrics=memory(),
        ),
        "negative setup time accepted",
    )

    empty_samples = terminal()
    require_raises(
        BrowserNormalizedLifecycleInvalid,
        lambda: attach_browser_normalized_evidence(
            empty_samples,
            setup_to_ready_seconds=1.0,
            query_samples_ms=[],
            warmup_query_count=0,
            memory_metrics=memory(),
        ),
        "empty measured sample set accepted",
    )

    warmup_bool = terminal()
    require_raises(
        BrowserNormalizedLifecycleInvalid,
        lambda: attach_browser_normalized_evidence(
            warmup_bool,
            setup_to_ready_seconds=1.0,
            query_samples_ms=[1.0],
            warmup_query_count=True,
            memory_metrics=memory(),
        ),
        "boolean warmup count accepted",
    )

    diagnostics_only = copy.deepcopy(memory())
    diagnostics_only["browser_scope_resident_mb_after_setup"] = 1000.0
    diagnostics_only["browser_scope_resident_mb_after_queries"] = 2000.0
    require(
        normalized_incremental_peak_memory_mb(diagnostics_only) == 12.5,
        "diagnostic resident snapshots contaminated normalized peak memory",
    )

    print("browser normalized lifecycle adapter tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
