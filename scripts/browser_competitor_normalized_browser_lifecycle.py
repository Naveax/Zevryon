#!/usr/bin/env python3
from __future__ import annotations

import math
from typing import Mapping, Sequence

from browser_competitor_benchmark_evidence import EvidenceIdentity
from browser_competitor_normalized_core_evidence import (
    NormalizedCoreEvidenceInvalid,
    attach_normalized_core_evidence,
    build_normalized_core_evidence,
)


NORMALIZED_SETUP_BOUNDARY = "case-owned-runtime-launch-to-post-warmup-ready-v1"
NORMALIZED_MEMORY_SCOPE = "case-owned-process-tree-above-pre-launch-baseline-v1"
_IDENTITY_KEYS = (
    "host_platform",
    "host_arch",
    "system_fingerprint",
    "harness_schema",
    "corpus_sha256",
    "scenario_fingerprint",
)


class BrowserNormalizedLifecycleInvalid(ValueError):
    pass


def _identity_from_terminal(terminal: Mapping[str, object]) -> EvidenceIdentity:
    values: dict[str, str] = {}
    for key in _IDENTITY_KEYS:
        value = terminal.get(key)
        if not isinstance(value, str) or not value.strip():
            raise BrowserNormalizedLifecycleInvalid(
                f"browser terminal evidence requires {key}"
            )
        values[key] = value
    return EvidenceIdentity(**values)


def normalized_incremental_peak_memory_mb(
    memory_metrics: Mapping[str, object],
) -> float:
    if memory_metrics.get("memory_metric_status") != "valid":
        raise BrowserNormalizedLifecycleInvalid(
            "browser normalized memory requires valid process-scope evidence"
        )
    peak = memory_metrics.get("browser_scope_peak_mb")
    if isinstance(peak, bool) or not isinstance(peak, (int, float)):
        raise BrowserNormalizedLifecycleInvalid(
            "browser normalized memory requires numeric process-scope peak"
        )
    normalized = float(peak)
    if not math.isfinite(normalized) or normalized <= 0.0:
        raise BrowserNormalizedLifecycleInvalid(
            "browser normalized process-scope peak must be finite and positive"
        )
    return normalized


def attach_browser_normalized_evidence(
    terminal: Mapping[str, object],
    *,
    setup_to_ready_seconds: float,
    query_samples_ms: Sequence[float],
    warmup_query_count: int,
    memory_metrics: Mapping[str, object],
) -> dict[str, object]:
    if terminal.get("status") != "success":
        raise BrowserNormalizedLifecycleInvalid(
            "browser normalized evidence requires a successful terminal record"
        )
    identity = _identity_from_terminal(terminal)
    peak_mb = normalized_incremental_peak_memory_mb(memory_metrics)
    try:
        evidence = build_normalized_core_evidence(
            identity,
            setup_to_ready_seconds=setup_to_ready_seconds,
            query_samples_ms=query_samples_ms,
            warmup_query_count=warmup_query_count,
            incremental_peak_memory_mb=peak_mb,
            setup_boundary=NORMALIZED_SETUP_BOUNDARY,
            memory_scope=NORMALIZED_MEMORY_SCOPE,
        )
        return attach_normalized_core_evidence(terminal, evidence)
    except NormalizedCoreEvidenceInvalid as exc:
        raise BrowserNormalizedLifecycleInvalid(str(exc)) from exc
