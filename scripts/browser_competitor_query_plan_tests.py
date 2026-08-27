#!/usr/bin/env python3
from __future__ import annotations

from browser_competitor_query_plan import (
    DEFAULT_WARMUP_QUERY_COUNT,
    NATIVE_OFFSET_SPACE,
    native_fraction,
    plan_query_offsets,
)
from browser_competitor_scenario_contract import deterministic_offsets


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def require_raises(exc_type, callable_, message: str) -> None:
    try:
        callable_()
    except exc_type:
        return
    raise AssertionError(message)


def main() -> int:
    virtual = plan_query_offsets(
        mode="virtualized",
        payload_bytes=64 * 1024 * 1024,
        virtual_slice_bytes=128 * 1024,
        query_count=7,
        warmup_query_count=3,
    )
    expected_virtual = tuple(
        deterministic_offsets(64 * 1024 * 1024, 128 * 1024, 10)
    )
    require(virtual.warmup_offsets == expected_virtual[:3], "virtual warmup prefix drifted")
    require(virtual.measured_offsets == expected_virtual[3:], "virtual measured suffix drifted")
    require(virtual.all_offsets == expected_virtual, "virtual combined query sequence drifted")
    require(virtual.warmup_query_count == 3, "virtual warmup count drifted")
    require(virtual.measured_query_count == 7, "virtual measured count drifted")

    default_warmup = plan_query_offsets(
        mode="virtualized",
        payload_bytes=4096,
        virtual_slice_bytes=512,
        query_count=2,
    )
    require(
        default_warmup.warmup_query_count == DEFAULT_WARMUP_QUERY_COUNT,
        "default warmup authority drifted",
    )

    zero_warmup = plan_query_offsets(
        mode="virtualized",
        payload_bytes=4096,
        virtual_slice_bytes=512,
        query_count=2,
        warmup_query_count=0,
    )
    require(zero_warmup.warmup_offsets == (), "zero-warmup plan emitted warmups")
    require(len(zero_warmup.measured_offsets) == 2, "zero-warmup measured count drifted")

    native_a = plan_query_offsets(
        mode="native-dom",
        payload_bytes=64 * 1024 * 1024,
        virtual_slice_bytes=128 * 1024,
        query_count=5,
        warmup_query_count=2,
    )
    native_b = plan_query_offsets(
        mode="native-dom",
        payload_bytes=96 * 1024 * 1024,
        virtual_slice_bytes=64 * 1024,
        query_count=5,
        warmup_query_count=2,
    )
    expected_native = tuple(deterministic_offsets(NATIVE_OFFSET_SPACE, 1, 7))
    require(native_a.all_offsets == expected_native, "native query sequence drifted")
    require(
        native_a.all_offsets == native_b.all_offsets,
        "native query sequence depended on virtual-only payload/slice dimensions",
    )

    require(native_fraction(0) == 0.0, "native zero fraction drifted")
    require(native_fraction(NATIVE_OFFSET_SPACE) == 1.0, "native unit fraction drifted")
    midpoint = native_fraction(NATIVE_OFFSET_SPACE // 2)
    require(midpoint == 0.5, "native midpoint fraction drifted")

    virtual_small = plan_query_offsets(
        mode="virtualized",
        payload_bytes=4096,
        virtual_slice_bytes=512,
        query_count=4,
        warmup_query_count=1,
    )
    virtual_large = plan_query_offsets(
        mode="virtualized",
        payload_bytes=8192,
        virtual_slice_bytes=512,
        query_count=4,
        warmup_query_count=1,
    )
    require(
        virtual_small.all_offsets != virtual_large.all_offsets,
        "virtual query plan ignored payload extent",
    )

    require_raises(
        ValueError,
        lambda: plan_query_offsets(
            mode="future",
            payload_bytes=1,
            virtual_slice_bytes=1,
            query_count=1,
        ),
        "unknown query-plan mode was accepted",
    )
    require_raises(
        ValueError,
        lambda: plan_query_offsets(
            mode="virtualized",
            payload_bytes=1,
            virtual_slice_bytes=1,
            query_count=1,
            warmup_query_count=True,
        ),
        "boolean warmup count was accepted",
    )
    require_raises(
        ValueError,
        lambda: plan_query_offsets(
            mode="virtualized",
            payload_bytes=1,
            virtual_slice_bytes=1,
            query_count=1,
            warmup_query_count=-1,
        ),
        "negative warmup count was accepted",
    )
    require_raises(ValueError, lambda: native_fraction(-1), "negative native offset was accepted")
    require_raises(
        ValueError,
        lambda: native_fraction(NATIVE_OFFSET_SPACE + 1),
        "oversized native offset was accepted",
    )

    print("competitor deterministic query-plan authority tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
