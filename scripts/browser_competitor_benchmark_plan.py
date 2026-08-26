#!/usr/bin/env python3
from __future__ import annotations

from dataclasses import dataclass
from typing import Iterable

from browser_competitor_registry import CompetitorSpec, resolve_requested


DEFAULT_COMPETITORS = ("chromium", "firefox")
BENCHMARK_MODES = ("virtualized", "native-dom")


@dataclass(frozen=True)
class BenchmarkCasePlan:
    competitor: str
    canonical_name: str
    canonical: bool
    adapter: str
    mode: str
    executable: bool
    reason: str | None = None


def _case_plan(spec: CompetitorSpec, mode: str) -> BenchmarkCasePlan:
    if mode not in BENCHMARK_MODES:
        raise ValueError(f"unknown benchmark mode: {mode}")
    executable = spec.adapter == "playwright"
    reason = None
    if not executable:
        reason = (
            f"adapter {spec.adapter} is not wired into the giant-document benchmark yet"
        )
    return BenchmarkCasePlan(
        competitor=spec.key,
        canonical_name=spec.canonical_name,
        canonical=spec.canonical,
        adapter=spec.adapter,
        mode=mode,
        executable=executable,
        reason=reason,
    )


def plan_benchmark_cases(
    competitors: Iterable[str] | None = None,
    modes: Iterable[str] = BENCHMARK_MODES,
) -> list[BenchmarkCasePlan]:
    requested_keys = list(DEFAULT_COMPETITORS if competitors is None else competitors)
    specs = resolve_requested(requested_keys)
    requested_modes = list(modes)
    if len(requested_modes) != len(set(requested_modes)):
        raise ValueError("duplicate benchmark mode request")
    if not requested_modes:
        raise ValueError("at least one benchmark mode is required")
    for mode in requested_modes:
        if mode not in BENCHMARK_MODES:
            raise ValueError(f"unknown benchmark mode: {mode}")

    return [
        _case_plan(spec, mode)
        for spec in specs
        for mode in requested_modes
    ]


def unsupported_case_record(
    plan: BenchmarkCasePlan,
    *,
    payload_bytes: int,
) -> dict[str, object]:
    if plan.executable or plan.reason is None:
        raise ValueError("unsupported case record requires a non-executable plan")
    if payload_bytes <= 0:
        raise ValueError("payload_bytes must be positive")
    return {
        "status": "unsupported",
        "competitor": plan.competitor,
        "browser": plan.competitor,
        "canonical_name": plan.canonical_name,
        "canonical": plan.canonical,
        "adapter": plan.adapter,
        "mode": plan.mode,
        "payload_bytes": payload_bytes,
        "reason": plan.reason,
    }
