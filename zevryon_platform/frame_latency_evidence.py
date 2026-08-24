from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
import math
from typing import Iterable, Sequence

from .benchmark_metadata import (
    BenchmarkMachineMetadata,
    ThermalState,
    physical_certification_checks,
)
from .performance_contract import DEVICE_PROFILES

SCHEMA_VERSION = 1
MIN_CERTIFICATION_SAMPLES = 1000
MAX_FRAME_SAMPLE_MS = 60_000.0


def _validated_samples(samples: Iterable[float]) -> tuple[float, ...]:
    values = tuple(float(value) for value in samples)
    if not values:
        raise ValueError("frame latency samples must not be empty")
    for value in values:
        if not math.isfinite(value) or value < 0.0 or value > MAX_FRAME_SAMPLE_MS:
            raise ValueError("frame latency sample outside evidence range")
    return values


def nearest_rank_percentile(samples: Sequence[float], percentile: float) -> float:
    if not (0.0 < percentile <= 100.0):
        raise ValueError("percentile must be in (0, 100]")
    values = sorted(_validated_samples(samples))
    rank = max(1, math.ceil((percentile / 100.0) * len(values)))
    return values[rank - 1]


def frame_sample_sha256(samples: Sequence[float]) -> str:
    values = _validated_samples(samples)
    digest = hashlib.sha256()
    for value in values:
        digest.update(f"{value:.9f}\n".encode("ascii"))
    return digest.hexdigest()


@dataclass(frozen=True)
class FrameLatencySummary:
    sample_count: int
    p50_ms: float
    p95_ms: float
    p99_ms: float
    maximum_ms: float
    frames_over_50ms: int
    frames_over_75ms: int
    sample_sha256: str

    def to_dict(self) -> dict[str, object]:
        return {
            "sample_count": self.sample_count,
            "p50_ms": self.p50_ms,
            "p95_ms": self.p95_ms,
            "p99_ms": self.p99_ms,
            "maximum_ms": self.maximum_ms,
            "frames_over_50ms": self.frames_over_50ms,
            "frames_over_75ms": self.frames_over_75ms,
            "sample_sha256": self.sample_sha256,
        }


@dataclass(frozen=True)
class FrameLatencyEvidence:
    schema_version: int
    machine: BenchmarkMachineMetadata
    summary: FrameLatencySummary
    p99_threshold_ms: float
    maximum_normal_stall_ms: float
    checks: dict[str, bool]

    @property
    def certified(self) -> bool:
        return bool(self.checks.get("frame_latency_certified", False))

    def to_dict(self) -> dict[str, object]:
        return {
            "schema_version": self.schema_version,
            "machine": self.machine.to_dict(),
            "summary": self.summary.to_dict(),
            "limits": {
                "p99_threshold_ms": self.p99_threshold_ms,
                "maximum_normal_stall_ms": self.maximum_normal_stall_ms,
                "minimum_samples": MIN_CERTIFICATION_SAMPLES,
            },
            "checks": dict(sorted(self.checks.items())),
        }

    def to_json(self) -> str:
        return json.dumps(self.to_dict(), sort_keys=True, separators=(",", ":"))


def summarize_frame_latencies(samples: Sequence[float]) -> FrameLatencySummary:
    values = _validated_samples(samples)
    return FrameLatencySummary(
        sample_count=len(values),
        p50_ms=nearest_rank_percentile(values, 50.0),
        p95_ms=nearest_rank_percentile(values, 95.0),
        p99_ms=nearest_rank_percentile(values, 99.0),
        maximum_ms=max(values),
        frames_over_50ms=sum(value > 50.0 for value in values),
        frames_over_75ms=sum(value > 75.0 for value in values),
        sample_sha256=frame_sample_sha256(values),
    )


def evaluate_frame_latencies(
    samples: Sequence[float],
    machine: BenchmarkMachineMetadata,
) -> FrameLatencyEvidence:
    machine.validate()
    summary = summarize_frame_latencies(samples)
    profile = DEVICE_PROFILES[machine.device_class]
    physical = physical_certification_checks(machine)
    checks = {
        "physical_metadata_complete": physical["physical_metadata_complete"],
        "thermal_state_stable": machine.thermal.state
        in {ThermalState.NOMINAL, ThermalState.FAIR},
        "minimum_sample_count": summary.sample_count >= MIN_CERTIFICATION_SAMPLES,
        "p99_within_profile": summary.p99_ms <= profile.scroll_p99_ms,
        "maximum_stall_within_profile": summary.maximum_ms
        <= profile.maximum_normal_stall_ms,
    }
    checks["frame_latency_certified"] = all(checks.values())
    return FrameLatencyEvidence(
        schema_version=SCHEMA_VERSION,
        machine=machine,
        summary=summary,
        p99_threshold_ms=profile.scroll_p99_ms,
        maximum_normal_stall_ms=float(profile.maximum_normal_stall_ms),
        checks=checks,
    )


def parse_frame_samples(text: str) -> tuple[float, ...]:
    stripped = text.strip()
    if not stripped:
        raise ValueError("frame sample input is empty")
    if stripped.startswith("["):
        parsed = json.loads(stripped)
        if not isinstance(parsed, list):
            raise ValueError("JSON frame sample input must be an array")
        return _validated_samples(parsed)
    tokens = stripped.replace(",", " ").split()
    return _validated_samples(float(token) for token in tokens)
