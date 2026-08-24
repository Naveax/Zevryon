from __future__ import annotations

from dataclasses import dataclass
import json
from typing import Mapping

from .frame_latency_evidence import FrameLatencyEvidence

SCHEMA_VERSION = 1


@dataclass(frozen=True)
class NativeFrameProbeSummary:
    operation: str
    profile: str
    frame_budget_us: int
    warmup_samples: int
    recorded_samples: int
    visible_layouts: int
    frame_overruns: int
    prefetch_accepts: int
    pool_thread_starts: int
    pool_ready_peak_bytes: int

    @classmethod
    def from_mapping(cls, value: Mapping[str, object]) -> "NativeFrameProbeSummary":
        required = {
            "operation",
            "profile",
            "frame_budget_us",
            "warmup_samples",
            "recorded_samples",
            "visible_layouts",
            "frame_overruns",
            "prefetch_accepts",
            "pool_thread_starts",
            "pool_ready_peak_bytes",
        }
        if set(value) != required:
            raise ValueError("native frame probe summary fields mismatch")
        operation = value["operation"]
        profile = value["profile"]
        if not isinstance(operation, str) or not isinstance(profile, str):
            raise ValueError("native frame probe string fields are invalid")
        numbers: dict[str, int] = {}
        for name in required - {"operation", "profile"}:
            raw = value[name]
            if not isinstance(raw, int) or isinstance(raw, bool) or raw < 0:
                raise ValueError(f"native frame probe {name} is invalid")
            numbers[name] = raw
        return cls(operation=operation, profile=profile, **numbers)

    @classmethod
    def from_json_line(cls, text: str) -> "NativeFrameProbeSummary":
        parsed = json.loads(text)
        if not isinstance(parsed, dict):
            raise ValueError("native frame probe output must be a JSON object")
        return cls.from_mapping(parsed)

    def to_dict(self) -> dict[str, object]:
        return {
            "operation": self.operation,
            "profile": self.profile,
            "frame_budget_us": self.frame_budget_us,
            "warmup_samples": self.warmup_samples,
            "recorded_samples": self.recorded_samples,
            "visible_layouts": self.visible_layouts,
            "frame_overruns": self.frame_overruns,
            "prefetch_accepts": self.prefetch_accepts,
            "pool_thread_starts": self.pool_thread_starts,
            "pool_ready_peak_bytes": self.pool_ready_peak_bytes,
        }


@dataclass(frozen=True)
class NativeFrameCertification:
    schema_version: int
    probe: NativeFrameProbeSummary
    frame_latency: FrameLatencyEvidence
    checks: dict[str, bool]

    @property
    def certified(self) -> bool:
        return bool(self.checks.get("native_frame_certified", False))

    def to_dict(self) -> dict[str, object]:
        return {
            "schema_version": self.schema_version,
            "probe": self.probe.to_dict(),
            "frame_latency": self.frame_latency.to_dict(),
            "checks": dict(sorted(self.checks.items())),
        }

    def to_json(self) -> str:
        return json.dumps(self.to_dict(), sort_keys=True, separators=(",", ":"))


def build_native_frame_certification(
    probe: NativeFrameProbeSummary,
    frame_latency: FrameLatencyEvidence,
    *,
    requested_samples: int,
    requested_warmup: int,
) -> NativeFrameCertification:
    if requested_samples < 1 or requested_warmup < 0:
        raise ValueError("requested native frame sample counts are invalid")
    expected_profile = frame_latency.machine.device_class.value
    checks = {
        "probe_operation_valid": probe.operation == "zenith-tab-runtime-frame-probe",
        "probe_profile_matches_machine": probe.profile == expected_profile,
        "probe_recorded_samples_match": probe.recorded_samples == requested_samples,
        "probe_warmup_samples_match": probe.warmup_samples == requested_warmup,
        "probe_visible_layouts_cover_run": probe.visible_layouts
        >= requested_samples + requested_warmup,
        "probe_worker_bound_respected": probe.pool_thread_starts <= 64,
        "frame_latency_certified": frame_latency.certified,
    }
    checks["native_frame_certified"] = all(checks.values())
    return NativeFrameCertification(
        schema_version=SCHEMA_VERSION,
        probe=probe,
        frame_latency=frame_latency,
        checks=checks,
    )
