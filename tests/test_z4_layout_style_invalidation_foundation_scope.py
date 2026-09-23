from __future__ import annotations

import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCOPE = ROOT / "certification" / "z4_layout_style_invalidation_foundation_scope.json"
PROGRAM = ROOT / "config" / "zenith_program.json"
HEADER = ROOT / "src" / "z4_layout_style_invalidation_v1.hpp"
SOURCE = ROOT / "src" / "z4_layout_style_invalidation_v1.cpp"
CMAKE = ROOT / "cmake" / "z4_layout_style_invalidation_v1.cmake"
ORACLE = ROOT / "tests" / "z4_layout_style_invalidation_v1_tests.cpp"


def test_z4_layout_style_invalidation_foundation_scope() -> None:
    scope = json.loads(SCOPE.read_text(encoding="utf-8"))
    assert scope["schema"] == "zevryon.z4-layout-style-invalidation-foundation.v1"
    assert scope["milestone"] == "Z4"
    assert scope["status_change"] is False
    assert scope["gate_progress"] == ["incremental_invalidation"]
    assert scope["admitted_gates"] == []
    assert scope["resource_class"] == "Layout"
    assert scope["hard_bounds"] == {
        "maximum_nodes_limit": 65_536,
        "maximum_ranges_limit": 65_536,
        "maximum_work_units_limit": 131_072,
    }

    program = json.loads(PROGRAM.read_text(encoding="utf-8"))
    z4 = next(item for item in program["milestones"] if item["id"] == "Z4")
    assert z4["required_gates"] == [
        "inline_layout_reference_tests",
        "block_layout_reference_tests",
        "incremental_invalidation",
        "scroll_anchor_error_bound",
        "warm_scroll_zero_allocation",
    ]
    assert "incremental_invalidation" in z4["required_gates"]

    header = HEADER.read_text(encoding="utf-8")
    source = SOURCE.read_text(encoding="utf-8")
    cmake = CMAKE.read_text(encoding="utf-8")
    oracle = ORACLE.read_text(encoding="utf-8")

    assert "compute_layout_style_invalidation_v1" in header
    assert "same canonical" in header
    assert "previous_terminal != current_terminal" in source
    assert "maximum_work_units" in source
    assert "LayoutStyleInvalidationRangeV1{101U, 103U}" in oracle
    assert "RangeLimitExceeded" in oracle
    assert "zevryon-z4-layout-style-invalidation-v1-tests" in cmake


if __name__ == "__main__":
    test_z4_layout_style_invalidation_foundation_scope()
    print("Z4 layout style invalidation foundation scope passed")
