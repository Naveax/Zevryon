#!/usr/bin/env python3
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Mapping

from browser_competitor_benchmark_plan import BenchmarkCasePlan, unsupported_case_record
from browser_competitor_evidence_context import EvidenceContext, canonical_json_bytes
from browser_competitor_registry import get_spec, terminal_record, validate_terminal_record


_EXECUTABLE_TERMINAL_STATES = frozenset(
    {"success", "unsupported", "unavailable", "timeout", "error", "invalid"}
)


@dataclass(frozen=True)
class RawCaseResult:
    status: str
    runtime_identity: str | None = None
    reason: str | None = None
    measurements: Mapping[str, object] = field(default_factory=dict)

    def validate(self) -> None:
        if self.status not in _EXECUTABLE_TERMINAL_STATES:
            raise ValueError(f"raw case result has invalid terminal state: {self.status}")
        if self.status == "success":
            if not isinstance(self.runtime_identity, str) or not self.runtime_identity.strip():
                raise ValueError("successful raw case result requires runtime identity")
            if self.reason is not None:
                raise ValueError("successful raw case result must not carry a failure reason")
        else:
            if not isinstance(self.reason, str) or not self.reason.strip():
                raise ValueError(f"{self.status} raw case result requires a reason")
        if not isinstance(self.measurements, Mapping):
            raise ValueError("raw case measurements must be a mapping")
        canonical_json_bytes(dict(self.measurements))


def _validate_plan_identity(plan: BenchmarkCasePlan) -> None:
    spec = get_spec(plan.competitor)
    if spec.canonical_name != plan.canonical_name:
        raise ValueError("benchmark plan canonical name drifted from registry")
    if spec.canonical is not plan.canonical:
        raise ValueError("benchmark plan canonical flag drifted from registry")
    if spec.adapter != plan.adapter:
        raise ValueError("benchmark plan adapter drifted from registry")


def terminalize_executable_case(
    plan: BenchmarkCasePlan,
    raw: RawCaseResult,
    context: EvidenceContext | None,
    *,
    payload_bytes: int,
) -> dict[str, object]:
    if not plan.executable:
        raise ValueError("executable terminalizer received a non-executable plan")
    if payload_bytes <= 0:
        raise ValueError("payload_bytes must be positive")
    _validate_plan_identity(plan)
    raw.validate()

    spec = get_spec(plan.competitor)
    if raw.status == "success":
        if context is None:
            raise ValueError("successful executable case requires evidence context")
        context.validate()
        record = terminal_record(
            spec,
            "success",
            runtime_identity=raw.runtime_identity,
            **context.terminal_kwargs(),
        )
    else:
        # Failure records intentionally remain terminal even when evidence-context
        # construction itself failed. The registry requires comparison identity only
        # for success; a non-success still needs an exact terminal status + reason.
        record = terminal_record(spec, raw.status, reason=raw.reason)

    validate_terminal_record(record)
    return {
        **record,
        "mode": plan.mode,
        "payload_bytes": payload_bytes,
        "measurements": dict(raw.measurements),
    }


def terminalize_unwired_case(
    plan: BenchmarkCasePlan,
    *,
    payload_bytes: int,
) -> dict[str, object]:
    if plan.executable:
        raise ValueError("unwired terminalizer received an executable plan")
    _validate_plan_identity(plan)
    record = unsupported_case_record(plan, payload_bytes=payload_bytes)
    validate_terminal_record(record)
    return {**record, "measurements": {}}
