#!/usr/bin/env python3
from __future__ import annotations

from dataclasses import asdict, dataclass
import hashlib
import json
import math
import re
from typing import Mapping, Sequence

from browser_competitor_registry import (
    CompetitorSpec,
    terminal_record,
    validate_terminal_record,
)


CASE_EVIDENCE_SCHEMA = "zevryon.competitor.case-evidence.v1"
CORE_METRIC_KEYS = (
    "setup_to_ready_seconds",
    "query_milliseconds_p50",
    "query_milliseconds_p95",
    "query_milliseconds_p99",
    "incremental_peak_memory_mb",
)
_SHA256_RE = re.compile(r"^[0-9a-f]{64}$")


@dataclass(frozen=True)
class SystemState:
    platform: str
    arch: str
    os_version: str
    kernel: str
    cpu_model: str
    logical_cpus: int
    total_memory_bytes: int
    graphics_identity: str
    graphics_driver: str
    power_mode: str
    thermal_state: str

    def validate(self) -> None:
        for field in (
            "platform",
            "arch",
            "os_version",
            "kernel",
            "cpu_model",
            "graphics_identity",
            "graphics_driver",
            "power_mode",
            "thermal_state",
        ):
            value = getattr(self, field)
            if not isinstance(value, str) or not value.strip():
                raise ValueError(f"system state requires {field}")
        if self.logical_cpus <= 0:
            raise ValueError("system state logical_cpus must be positive")
        if self.total_memory_bytes <= 0:
            raise ValueError("system state total_memory_bytes must be positive")

    def record(self) -> dict[str, object]:
        self.validate()
        return asdict(self)


@dataclass(frozen=True)
class BenchmarkScenario:
    mode: str
    payload_bytes: int
    measured_query_count: int
    warmup_query_count: int
    virtual_slice_bytes: int | None
    viewport_width: int
    viewport_height: int
    deterministic_sequence_id: str
    unicode_generator_schema: str
    setup_boundary: str
    memory_scope: str
    timeout_seconds: int

    def validate(self) -> None:
        if self.mode not in {"virtualized", "native-dom"}:
            raise ValueError(f"unknown benchmark mode: {self.mode}")
        if self.payload_bytes <= 0:
            raise ValueError("scenario payload_bytes must be positive")
        if self.measured_query_count <= 0:
            raise ValueError("scenario measured_query_count must be positive")
        if self.warmup_query_count < 0:
            raise ValueError("scenario warmup_query_count must be non-negative")
        if self.mode == "virtualized":
            if self.virtual_slice_bytes is None or self.virtual_slice_bytes <= 0:
                raise ValueError("virtualized scenario requires positive virtual_slice_bytes")
        elif self.virtual_slice_bytes is not None:
            raise ValueError("native-dom scenario must not declare virtual_slice_bytes")
        if self.viewport_width <= 0 or self.viewport_height <= 0:
            raise ValueError("scenario viewport dimensions must be positive")
        if self.timeout_seconds <= 0:
            raise ValueError("scenario timeout_seconds must be positive")
        for field in (
            "deterministic_sequence_id",
            "unicode_generator_schema",
            "setup_boundary",
            "memory_scope",
        ):
            value = getattr(self, field)
            if not isinstance(value, str) or not value.strip():
                raise ValueError(f"scenario requires {field}")

    def record(self) -> dict[str, object]:
        self.validate()
        return asdict(self)


def canonical_fingerprint(payload: Mapping[str, object]) -> str:
    try:
        encoded = json.dumps(
            payload,
            ensure_ascii=False,
            allow_nan=False,
            separators=(",", ":"),
            sort_keys=True,
        ).encode("utf-8")
    except (TypeError, ValueError) as exc:
        raise ValueError("fingerprint payload must be canonical JSON data") from exc
    return hashlib.sha256(encoded).hexdigest()


def percentile(values: Sequence[float], percent: float) -> float:
    if not values:
        raise ValueError("percentile requires samples")
    if not math.isfinite(percent) or percent < 0.0 or percent > 100.0:
        raise ValueError("percentile must be within [0, 100]")
    ordered = sorted(float(value) for value in values)
    if any(not math.isfinite(value) or value < 0.0 for value in ordered):
        raise ValueError("query samples must be finite and non-negative")
    position = (len(ordered) - 1) * percent / 100.0
    lower = int(position)
    upper = min(lower + 1, len(ordered) - 1)
    fraction = position - lower
    return ordered[lower] * (1.0 - fraction) + ordered[upper] * fraction


def _nonnegative_finite(value: float, field: str) -> float:
    normalized = float(value)
    if not math.isfinite(normalized) or normalized < 0.0:
        raise ValueError(f"{field} must be finite and non-negative")
    return normalized


def _require_sha256(value: str, field: str) -> str:
    if _SHA256_RE.fullmatch(value) is None:
        raise ValueError(f"{field} must be a lowercase SHA-256")
    return value


def build_measurement_evidence(
    *,
    system_state: SystemState,
    harness_schema: str,
    corpus_sha256: str,
    scenario: BenchmarkScenario,
    setup_to_ready_seconds: float,
    query_samples_ms: Sequence[float],
    incremental_peak_memory_mb: float,
) -> dict[str, object]:
    system_record = system_state.record()
    scenario_record = scenario.record()
    if not isinstance(harness_schema, str) or not harness_schema.strip():
        raise ValueError("measurement evidence requires harness_schema")
    corpus_sha256 = _require_sha256(corpus_sha256, "corpus_sha256")

    samples = [_nonnegative_finite(value, "query sample") for value in query_samples_ms]
    if len(samples) != scenario.measured_query_count:
        raise ValueError("query sample count does not match scenario")

    core_metrics = {
        "setup_to_ready_seconds": _nonnegative_finite(
            setup_to_ready_seconds, "setup_to_ready_seconds"
        ),
        "query_milliseconds_p50": percentile(samples, 50.0),
        "query_milliseconds_p95": percentile(samples, 95.0),
        "query_milliseconds_p99": percentile(samples, 99.0),
        "incremental_peak_memory_mb": _nonnegative_finite(
            incremental_peak_memory_mb, "incremental_peak_memory_mb"
        ),
    }
    evidence: dict[str, object] = {
        "evidence_schema": CASE_EVIDENCE_SCHEMA,
        "host_platform": system_state.platform.strip(),
        "host_arch": system_state.arch.strip(),
        "system_fingerprint": canonical_fingerprint(system_record),
        "harness_schema": harness_schema.strip(),
        "corpus_sha256": corpus_sha256,
        "scenario_fingerprint": canonical_fingerprint(scenario_record),
        "system_state": system_record,
        "scenario": scenario_record,
        "core_metrics": core_metrics,
        "query_samples_ms": samples,
        "query_sample_count": len(samples),
    }
    validate_measurement_evidence(evidence)
    return evidence


def validate_measurement_evidence(evidence: Mapping[str, object]) -> None:
    if evidence.get("evidence_schema") != CASE_EVIDENCE_SCHEMA:
        raise ValueError("measurement evidence schema mismatch")
    system_state = evidence.get("system_state")
    scenario = evidence.get("scenario")
    if not isinstance(system_state, Mapping) or not isinstance(scenario, Mapping):
        raise ValueError("measurement evidence requires system_state and scenario")
    if evidence.get("system_fingerprint") != canonical_fingerprint(system_state):
        raise ValueError("system fingerprint does not match system_state")
    if evidence.get("scenario_fingerprint") != canonical_fingerprint(scenario):
        raise ValueError("scenario fingerprint does not match scenario")
    if evidence.get("host_platform") != system_state.get("platform"):
        raise ValueError("host platform does not match system_state")
    if evidence.get("host_arch") != system_state.get("arch"):
        raise ValueError("host architecture does not match system_state")
    harness_schema = evidence.get("harness_schema")
    if not isinstance(harness_schema, str) or not harness_schema.strip():
        raise ValueError("measurement evidence requires harness_schema")
    corpus = evidence.get("corpus_sha256")
    if not isinstance(corpus, str):
        raise ValueError("measurement evidence requires corpus_sha256")
    _require_sha256(corpus, "corpus_sha256")

    metrics = evidence.get("core_metrics")
    samples_value = evidence.get("query_samples_ms")
    if not isinstance(metrics, Mapping) or set(metrics) != set(CORE_METRIC_KEYS):
        raise ValueError("measurement evidence core metric set mismatch")
    if not isinstance(samples_value, list) or not samples_value:
        raise ValueError("measurement evidence requires raw query samples")
    samples = [_nonnegative_finite(value, "query sample") for value in samples_value]
    sample_count = evidence.get("query_sample_count")
    if sample_count != len(samples):
        raise ValueError("query_sample_count does not match raw samples")
    scenario_count = scenario.get("measured_query_count")
    if scenario_count != len(samples):
        raise ValueError("scenario measured_query_count does not match raw samples")

    expected = {
        "query_milliseconds_p50": percentile(samples, 50.0),
        "query_milliseconds_p95": percentile(samples, 95.0),
        "query_milliseconds_p99": percentile(samples, 99.0),
    }
    for key in CORE_METRIC_KEYS:
        value = metrics.get(key)
        if not isinstance(value, (int, float)):
            raise ValueError(f"core metric {key} must be numeric")
        normalized = _nonnegative_finite(float(value), key)
        if key in expected and not math.isclose(
            normalized, expected[key], rel_tol=1e-12, abs_tol=1e-9
        ):
            raise ValueError(f"core metric {key} does not match raw samples")


def build_competitor_success_record(
    spec: CompetitorSpec,
    *,
    runtime_identity: str,
    evidence: Mapping[str, object],
) -> dict[str, object]:
    validate_measurement_evidence(evidence)
    record = terminal_record(
        spec,
        "success",
        runtime_identity=runtime_identity,
        host_platform=str(evidence["host_platform"]),
        host_arch=str(evidence["host_arch"]),
        system_fingerprint=str(evidence["system_fingerprint"]),
        harness_schema=str(evidence["harness_schema"]),
        corpus_sha256=str(evidence["corpus_sha256"]),
        scenario_fingerprint=str(evidence["scenario_fingerprint"]),
    )
    record.update(evidence)
    validate_terminal_record(record)
    validate_measurement_evidence(record)
    return record
