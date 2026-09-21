from __future__ import annotations

import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCOPE = ROOT / "certification" / "z3_css_offscreen_selection_foundation_scope.json"
PROGRAM = ROOT / "config" / "zenith_program.json"
HEADER = ROOT / "src" / "css_offscreen_style_selection_v1.hpp"
SOURCE = ROOT / "src" / "css_offscreen_style_selection_v1.cpp"
CMAKE = ROOT / "cmake" / "css_offscreen_style_selection_v1.cmake"


def test_z3_css_offscreen_selection_foundation_scope() -> None:
    scope = json.loads(SCOPE.read_text(encoding="utf-8"))
    assert scope["schema"] == "zevryon.z3-css-offscreen-selection-foundation.v1"
    assert scope["milestone"] == "Z3"
    assert scope["status_change"] is False
    assert scope["gate_progress"] == ["offscreen_materialization_bound"]
    assert scope["admitted_gates"] == []
    assert scope["resource_class"] == "ComputedStyle"
    assert scope["hard_bounds"] == {
        "maximum_candidates_limit": 16_384,
        "maximum_selected_styles_limit": 16_384,
        "maximum_lookahead_css_px_limit": 1_000_000,
        "maximum_work_units_limit": 67_108_864,
    }
    assert "duplicate terminal styles collapse to their nearest offscreen occurrence" in scope[
        "selection_policy"
    ]
    assert "selection output feeds materialize_css_style_nodes_v1 directly" in scope[
        "certified_foundation"
    ]

    program = json.loads(PROGRAM.read_text(encoding="utf-8"))
    z3 = next(item for item in program["milestones"] if item["id"] == "Z3")
    assert z3["status"] == "planned"
    assert z3["evidence"] == []
    assert "offscreen_materialization_bound" in z3["required_gates"]

    header = HEADER.read_text(encoding="utf-8")
    source = SOURCE.read_text(encoding="utf-8")
    cmake = CMAKE.read_text(encoding="utf-8")
    assert "select_css_offscreen_style_nodes_v1" in header
    assert "CssOffscreenStyleCandidateV1" in header
    assert "maximum_selected_styles" in header
    assert "lookahead_css_px" in header
    assert "duplicate_terminal_candidates" in source
    assert "maximum_work_units" in source
    assert "zevryon-css-offscreen-style-selection-v1-tests" in cmake


if __name__ == "__main__":
    test_z3_css_offscreen_selection_foundation_scope()
    print("Z3 CSS offscreen selection foundation scope contract passed")
