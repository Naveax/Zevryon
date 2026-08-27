#!/usr/bin/env python3
from __future__ import annotations

import copy

from browser_competitor_benchmark_evidence import EvidenceIdentity
from browser_competitor_normalized_core_evidence import (
    CORE_METRIC_KEYS,
    NormalizedCoreEvidenceInvalid,
    assert_comparable_core_evidence,
    attach_normalized_core_evidence,
    build_normalized_core_evidence,
    validate_normalized_core_evidence,
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def require_invalid(callable_, message: str) -> None:
    try:
        callable_()
    except NormalizedCoreEvidenceInvalid:
        return
    raise AssertionError(message)


def identity(seed: str = "a") -> EvidenceIdentity:
    return EvidenceIdentity(
        host_platform="Linux",
        host_arch="x86_64",
        system_fingerprint=seed * 64,
        harness_schema="zevryon.competitor.giant-document.v2",
        corpus_sha256="b" * 64,
        scenario_fingerprint="c" * 64,
    )


def evidence(seed: str = "a") -> dict[str, object]:
    return build_normalized_core_evidence(
        identity(seed),
        setup_to_ready_seconds=1.25,
        query_samples_ms=[1.0, 2.0, 3.0, 4.0, 5.0],
        warmup_query_count=2,
        incremental_peak_memory_mb=42.5,
        setup_boundary="cold-case-start-to-first-measured-query-ready-v1",
        memory_scope="owned-process-tree-above-pre-case-baseline-v1",
    )


def main() -> int:
    first = evidence()
    validate_normalized_core_evidence(first)
    metrics = first["core_metrics"]
    require(isinstance(metrics, dict), "core metrics missing")
    require(set(metrics) == set(CORE_METRIC_KEYS), "core metric set drifted")
    require(metrics["query_milliseconds_p50"] == 3.0, "p50 drifted")
    require(metrics["query_milliseconds_p95"] == 4.8, "p95 drifted")
    require(metrics["query_milliseconds_p99"] == 4.96, "p99 drifted")
    require(first["query_sample_count"] == 5, "raw sample count drifted")
    require(
        first["system_fingerprint"] == "a" * 64,
        "canonical system fingerprint was not reused",
    )

    second = evidence()
    assert_comparable_core_evidence([first, second])

    drifted_identity = evidence("d")
    require_invalid(
        lambda: assert_comparable_core_evidence([first, drifted_identity]),
        "system identity drift remained comparable",
    )

    drifted_boundary = copy.deepcopy(first)
    drifted_boundary["setup_boundary"] = "different-boundary"
    require_invalid(
        lambda: assert_comparable_core_evidence([first, drifted_boundary]),
        "setup boundary drift remained comparable",
    )

    drifted_memory = copy.deepcopy(first)
    drifted_memory["memory_scope"] = "different-memory-scope"
    require_invalid(
        lambda: assert_comparable_core_evidence([first, drifted_memory]),
        "memory scope drift remained comparable",
    )

    tampered_percentile = copy.deepcopy(first)
    tampered_percentile["core_metrics"]["query_milliseconds_p95"] = 1.0
    require_invalid(
        lambda: validate_normalized_core_evidence(tampered_percentile),
        "tampered percentile was accepted",
    )

    missing_sample = copy.deepcopy(first)
    missing_sample["query_samples_ms"].pop()
    require_invalid(
        lambda: validate_normalized_core_evidence(missing_sample),
        "sample-count mismatch was accepted",
    )

    warmups_included = copy.deepcopy(first)
    warmups_included["warmups_excluded_from_percentiles"] = False
    require_invalid(
        lambda: validate_normalized_core_evidence(warmups_included),
        "warmup contamination was accepted",
    )

    extra_metric = copy.deepcopy(first)
    extra_metric["core_metrics"]["query_milliseconds_mean"] = 3.0
    require_invalid(
        lambda: validate_normalized_core_evidence(extra_metric),
        "post-hoc core metric expansion was accepted",
    )

    require_invalid(
        lambda: build_normalized_core_evidence(
            identity(),
            setup_to_ready_seconds=1.0,
            query_samples_ms=[],
            warmup_query_count=0,
            incremental_peak_memory_mb=1.0,
            setup_boundary="setup",
            memory_scope="memory",
        ),
        "empty raw query evidence was accepted",
    )

    terminal = {
        "status": "success",
        **identity().as_terminal_kwargs(),
        "competitor": "chrome",
    }
    attached = attach_normalized_core_evidence(terminal, first)
    require(
        attached.get("normalized_core_evidence") == first,
        "normalized evidence attachment was lost",
    )

    bad_terminal = dict(terminal)
    bad_terminal["scenario_fingerprint"] = "e" * 64
    require_invalid(
        lambda: attach_normalized_core_evidence(bad_terminal, first),
        "terminal/evidence identity drift was accepted",
    )

    failed_terminal = dict(terminal)
    failed_terminal["status"] = "error"
    require_invalid(
        lambda: attach_normalized_core_evidence(failed_terminal, first),
        "normalized evidence attached to a failed terminal result",
    )

    print("normalized core evidence authority: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
