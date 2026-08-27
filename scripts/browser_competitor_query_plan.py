#!/usr/bin/env python3
from __future__ import annotations

from dataclasses import dataclass

from browser_competitor_scenario_contract import deterministic_offsets


DEFAULT_WARMUP_QUERY_COUNT = 3
NATIVE_OFFSET_SPACE = 1_000_000


@dataclass(frozen=True)
class DeterministicQueryPlan:
    mode: str
    warmup_offsets: tuple[int, ...]
    measured_offsets: tuple[int, ...]

    @property
    def warmup_query_count(self) -> int:
        return len(self.warmup_offsets)

    @property
    def measured_query_count(self) -> int:
        return len(self.measured_offsets)

    @property
    def all_offsets(self) -> tuple[int, ...]:
        return self.warmup_offsets + self.measured_offsets


def _nonnegative_int(value: object, field: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise ValueError(f"{field} must be a non-negative integer")
    return value


def plan_query_offsets(
    *,
    mode: str,
    payload_bytes: int,
    virtual_slice_bytes: int,
    query_count: int,
    warmup_query_count: int = DEFAULT_WARMUP_QUERY_COUNT,
) -> DeterministicQueryPlan:
    if mode not in {"virtualized", "native-dom"}:
        raise ValueError(f"unknown benchmark mode: {mode}")
    if (
        isinstance(payload_bytes, bool)
        or not isinstance(payload_bytes, int)
        or payload_bytes <= 0
    ):
        raise ValueError("payload_bytes must be a positive integer")
    if (
        isinstance(virtual_slice_bytes, bool)
        or not isinstance(virtual_slice_bytes, int)
        or virtual_slice_bytes <= 0
    ):
        raise ValueError("virtual_slice_bytes must be a positive integer")
    if (
        isinstance(query_count, bool)
        or not isinstance(query_count, int)
        or query_count <= 0
    ):
        raise ValueError("query_count must be a positive integer")
    warmup_count = _nonnegative_int(warmup_query_count, "warmup_query_count")
    total_count = warmup_count + query_count

    if mode == "virtualized":
        offsets = deterministic_offsets(
            payload_bytes,
            virtual_slice_bytes,
            total_count,
        )
    else:
        offsets = deterministic_offsets(NATIVE_OFFSET_SPACE, 1, total_count)

    if len(offsets) != total_count:
        raise RuntimeError("deterministic offset generator returned the wrong sample count")
    return DeterministicQueryPlan(
        mode=mode,
        warmup_offsets=tuple(offsets[:warmup_count]),
        measured_offsets=tuple(offsets[warmup_count:]),
    )


def native_fraction(offset: int) -> float:
    if isinstance(offset, bool) or not isinstance(offset, int):
        raise ValueError("native offset must be an integer")
    if offset < 0 or offset > NATIVE_OFFSET_SPACE:
        raise ValueError("native offset is outside the canonical fraction space")
    return offset / float(NATIVE_OFFSET_SPACE)
