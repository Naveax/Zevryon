#!/usr/bin/env python3
from __future__ import annotations

from dataclasses import dataclass
from typing import Callable, Iterable

from browser_competitor_benchmark_executor import (
    RawCaseResult,
    terminalize_executable_case,
    terminalize_unwired_case,
)
from browser_competitor_benchmark_plan import BenchmarkCasePlan
from browser_competitor_evidence_context import EvidenceContext


@dataclass(frozen=True)
class BenchmarkTimeouts:
    virtualized_seconds: int
    native_dom_seconds: int

    def validate(self) -> None:
        if self.virtualized_seconds <= 0:
            raise ValueError("virtualized timeout must be positive")
        if self.native_dom_seconds <= 0:
            raise ValueError("native DOM timeout must be positive")

    def for_mode(self, mode: str) -> int:
        self.validate()
        if mode == "virtualized":
            return self.virtualized_seconds
        if mode == "native-dom":
            return self.native_dom_seconds
        raise ValueError(f"unknown benchmark mode: {mode}")


ContextFactory = Callable[[BenchmarkCasePlan], EvidenceContext]
CaseRunner = Callable[[BenchmarkCasePlan, int], RawCaseResult]


def execute_benchmark_matrix(
    plans: Iterable[BenchmarkCasePlan],
    *,
    payload_bytes: int,
    timeouts: BenchmarkTimeouts,
    context_for: ContextFactory,
    run_case: CaseRunner,
) -> list[dict[str, object]]:
    if payload_bytes <= 0:
        raise ValueError("payload_bytes must be positive")
    timeouts.validate()
    ordered = list(plans)
    if not ordered:
        raise ValueError("benchmark matrix must contain at least one case")

    seen: set[tuple[str, str]] = set()
    output: list[dict[str, object]] = []
    for plan in ordered:
        key = (plan.competitor, plan.mode)
        if key in seen:
            raise ValueError(
                f"duplicate benchmark case plan: {plan.competitor}/{plan.mode}"
            )
        seen.add(key)

        if not plan.executable:
            output.append(terminalize_unwired_case(plan, payload_bytes=payload_bytes))
            continue

        timeout_seconds = timeouts.for_mode(plan.mode)
        context = context_for(plan)
        raw = run_case(plan, timeout_seconds)
        record = terminalize_executable_case(
            plan,
            raw,
            context,
            payload_bytes=payload_bytes,
        )
        record["timeout_seconds"] = timeout_seconds
        output.append(record)

    return output
