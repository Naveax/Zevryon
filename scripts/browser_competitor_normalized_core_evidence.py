#!/usr/bin/env python3
from __future__ import annotations

import math
import re
from typing import Mapping, Sequence

from browser_competitor_benchmark_evidence import EvidenceIdentity


NORMALIZED_CORE_EVIDENCE_SCHEMA = "zevryon.competitor.normalized-core-evidence.v1"
CORE_METRIC_KEYS = (
    "setup_to_ready_seconds",
    "query_milliseconds_p50",
    "query_milliseconds_p95",
    "query_milliseconds_p99",
    "incremental_peak_memory_mb",
)
CORE_METRIC_UNITS = {
    "setup_to_ready_seconds": "seconds",
    "query_milliseconds_p50": "milliseconds",
    "query_milliseconds_p95": "milliseconds",
    "query_milliseconds_p99": "milliseconds",
    "incremental_peak_memory_mb": "decimal-megabytes",
}
IDENTITY_KEYS = (
    "host_platform",
    "host_arch",
    "system_fingerprint",
    "harness_schema",
    "corpus_sha256",
    "scenario_fingerprint",
)
_SHA256_RE = re.compile(r"^[0-9a-f]{64}$")


class NormalizedCoreEvidenceInvalid(ValueError):
    pass


def _finite_nonnegative(value: object, field: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise NormalizedCoreEvidenceInvalid(f"{field} must be numeric")
    normalized = float(value)
    if not math.isfinite(normalized) or normalized < 0.0:
        raise NormalizedCoreEvidenceInvalid(
            f"{field} must be finite and non-negative"
        )
    return normalized


def percentile(values: Sequence[float], percent: float) -> float:
    if not values:
        raise NormalizedCoreEvidenceInvalid("percentile requires query samples")
    if not math.isfinite(percent) or percent < 0.0 or percent > 100.0:
        raise NormalizedCoreEvidenceInvalid("percentile must be within [0, 100]")
    ordered = sorted(_finite_nonnegative(value, "query sample") for value in values)
    position = (len(ordered) - 1) * percent / 100.0
    lower = int(position)
    upper = min(lower + 1, len(ordered) - 1)
    fraction = position - lower
    return ordered[lower] * (1.0 - fraction) + ordered[upper] * fraction


def _identity_record(identity: EvidenceIdentity) -> dict[str, str]:
    record = identity.as_terminal_kwargs()
    for key in IDENTITY_KEYS:
        value = record.get(key)
        if not isinstance(value, str) or not value.strip():
            raise NormalizedCoreEvidenceInvalid(
                f"normalized evidence identity requires {key}"
            )
    for key in ("system_fingerprint", "corpus_sha256", "scenario_fingerprint"):
        if _SHA256_RE.fullmatch(record[key]) is None:
            raise NormalizedCoreEvidenceInvalid(f"{key} must be a lowercase SHA-256")
    return record


def build_normalized_core_evidence(
    identity: EvidenceIdentity,
    *,
    setup_to_ready_seconds: float,
    query_samples_ms: Sequence[float],
    warmup_query_count: int,
    incremental_peak_memory_mb: float,
    setup_boundary: str,
    memory_scope: str,
) -> dict[str, object]:
    identity_record = _identity_record(identity)
    if isinstance(warmup_query_count, bool) or not isinstance(warmup_query_count, int):
        raise NormalizedCoreEvidenceInvalid("warmup_query_count must be an integer")
    if warmup_query_count < 0:
        raise NormalizedCoreEvidenceInvalid("warmup_query_count must be non-negative")
    if not isinstance(setup_boundary, str) or not setup_boundary.strip():
        raise NormalizedCoreEvidenceInvalid("setup_boundary must be declared")
    if not isinstance(memory_scope, str) or not memory_scope.strip():
        raise NormalizedCoreEvidenceInvalid("memory_scope must be declared")

    samples = [_finite_nonnegative(value, "query sample") for value in query_samples_ms]
    if not samples:
        raise NormalizedCoreEvidenceInvalid("query_samples_ms cannot be empty")

    metrics = {
        "setup_to_ready_seconds": _finite_nonnegative(
            setup_to_ready_seconds, "setup_to_ready_seconds"
        ),
        "query_milliseconds_p50": percentile(samples, 50.0),
        "query_milliseconds_p95": percentile(samples, 95.0),
        "query_milliseconds_p99": percentile(samples, 99.0),
        "incremental_peak_memory_mb": _finite_nonnegative(
            incremental_peak_memory_mb, "incremental_peak_memory_mb"
        ),
    }
    evidence: dict[str, object] = {
        "schema": NORMALIZED_CORE_EVIDENCE_SCHEMA,
        **identity_record,
        "setup_boundary": setup_boundary.strip(),
        "memory_scope": memory_scope.strip(),
        "warmup_query_count": warmup_query_count,
        "warmups_excluded_from_percentiles": True,
        "query_samples_ms": samples,
        "query_sample_count": len(samples),
        "core_metric_units": dict(CORE_METRIC_UNITS),
        "core_metrics": metrics,
    }
    validate_normalized_core_evidence(evidence)
    return evidence


def validate_normalized_core_evidence(evidence: Mapping[str, object]) -> None:
    if evidence.get("schema") != NORMALIZED_CORE_EVIDENCE_SCHEMA:
        raise NormalizedCoreEvidenceInvalid("normalized evidence schema mismatch")

    for key in IDENTITY_KEYS:
        value = evidence.get(key)
        if not isinstance(value, str) or not value.strip():
            raise NormalizedCoreEvidenceInvalid(f"normalized evidence requires {key}")
    for key in ("system_fingerprint", "corpus_sha256", "scenario_fingerprint"):
        value = evidence.get(key)
        if not isinstance(value, str) or _SHA256_RE.fullmatch(value) is None:
            raise NormalizedCoreEvidenceInvalid(f"{key} must be a lowercase SHA-256")

    setup_boundary = evidence.get("setup_boundary")
    memory_scope = evidence.get("memory_scope")
    if not isinstance(setup_boundary, str) or not setup_boundary.strip():
        raise NormalizedCoreEvidenceInvalid("normalized evidence requires setup_boundary")
    if not isinstance(memory_scope, str) or not memory_scope.strip():
        raise NormalizedCoreEvidenceInvalid("normalized evidence requires memory_scope")

    warmup_count = evidence.get("warmup_query_count")
    if (
        isinstance(warmup_count, bool)
        or not isinstance(warmup_count, int)
        or warmup_count < 0
    ):
        raise NormalizedCoreEvidenceInvalid(
            "warmup_query_count must be a non-negative integer"
        )
    if evidence.get("warmups_excluded_from_percentiles") is not True:
        raise NormalizedCoreEvidenceInvalid(
            "normalized percentiles must exclude declared warmup queries"
        )

    raw_samples = evidence.get("query_samples_ms")
    if not isinstance(raw_samples, list) or not raw_samples:
        raise NormalizedCoreEvidenceInvalid("normalized evidence requires raw query samples")
    samples = [_finite_nonnegative(value, "query sample") for value in raw_samples]
    sample_count = evidence.get("query_sample_count")
    if (
        isinstance(sample_count, bool)
        or not isinstance(sample_count, int)
        or sample_count != len(samples)
    ):
        raise NormalizedCoreEvidenceInvalid(
            "query_sample_count must match raw query samples"
        )

    units = evidence.get("core_metric_units")
    if not isinstance(units, Mapping) or dict(units) != CORE_METRIC_UNITS:
        raise NormalizedCoreEvidenceInvalid("core metric units mismatch")

    metrics = evidence.get("core_metrics")
    if not isinstance(metrics, Mapping) or set(metrics) != set(CORE_METRIC_KEYS):
        raise NormalizedCoreEvidenceInvalid("core metric set mismatch")
    normalized_metrics = {
        key: _finite_nonnegative(metrics.get(key), key) for key in CORE_METRIC_KEYS
    }
    expected_percentiles = {
        "query_milliseconds_p50": percentile(samples, 50.0),
        "query_milliseconds_p95": percentile(samples, 95.0),
        "query_milliseconds_p99": percentile(samples, 99.0),
    }
    for key, expected in expected_percentiles.items():
        if not math.isclose(
            normalized_metrics[key], expected, rel_tol=1e-12, abs_tol=1e-9
        ):
            raise NormalizedCoreEvidenceInvalid(
                f"{key} does not match raw query samples"
            )


def assert_comparable_core_evidence(
    records: Sequence[Mapping[str, object]],
) -> None:
    if not records:
        raise NormalizedCoreEvidenceInvalid("comparability requires evidence records")
    for record in records:
        validate_normalized_core_evidence(record)

    authority_keys = (
        *IDENTITY_KEYS,
        "setup_boundary",
        "memory_scope",
        "warmup_query_count",
        "warmups_excluded_from_percentiles",
        "query_sample_count",
        "core_metric_units",
    )
    reference = records[0]
    for index, record in enumerate(records[1:], start=1):
        drift = [key for key in authority_keys if record.get(key) != reference.get(key)]
        if drift:
            raise NormalizedCoreEvidenceInvalid(
                f"normalized evidence record {index} is not comparable: "
                + ", ".join(drift)
            )


def attach_normalized_core_evidence(
    terminal: Mapping[str, object],
    evidence: Mapping[str, object],
) -> dict[str, object]:
    validate_normalized_core_evidence(evidence)
    if terminal.get("status") != "success":
        raise NormalizedCoreEvidenceInvalid(
            "normalized core evidence may attach only to success terminal evidence"
        )
    drift = [key for key in IDENTITY_KEYS if terminal.get(key) != evidence.get(key)]
    if drift:
        raise NormalizedCoreEvidenceInvalid(
            "normalized evidence identity drifted from terminal evidence: "
            + ", ".join(drift)
        )
    return {**terminal, "normalized_core_evidence": dict(evidence)}
