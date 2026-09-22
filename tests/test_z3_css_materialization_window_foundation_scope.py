from __future__ import annotations

import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCOPE = ROOT / "certification" / "z3_css_materialization_window_foundation_scope.json"
PROGRAM = ROOT / "config" / "zenith_program.json"
HEADER = ROOT / "src" / "css_style_materialization_window_v1.hpp"
SOURCE = ROOT / "src" / "css_style_materialization_window_v1.cpp"
CMAKE = ROOT / "cmake" / "css_style_materialization_window_v1.cmake"
ORACLE = ROOT / "tests" / "css_style_materialization_window_v1_tests.cpp"


def test_z3_css_materialization_window_foundation_scope() -> None:
    scope = json.loads(SCOPE.read_text(encoding="utf-8"))
    assert scope["schema"] == "zevryon.z3-css-materialization-window-foundation.v1"
    assert scope["milestone"] == "Z3"
    assert scope["status_change"] is False
    assert scope["gate_progress"] == ["offscreen_materialization_bound"]
    assert scope["admitted_gates"] == []
    assert scope["resource_class"] == "ComputedStyle"
    assert scope["hard_bounds"] == {
        "maximum_visible_nodes_limit": 65_536,
        "maximum_offscreen_nodes_per_side_limit": 65_536,
        "maximum_selection_work_units_limit": 65_536,
    }
    assert (
        "caller supplies total logical document node count without a complete document terminal-style array"
        in scope["selection_model"]
    )
    assert (
        "selection takes one contiguous subspan of the bounded candidate and does not scan unrelated document nodes"
        in scope["selection_model"]
    )

    program = json.loads(PROGRAM.read_text(encoding="utf-8"))
    z3 = next(item for item in program["milestones"] if item["id"] == "Z3")
    assert z3["status"] == "planned"
    assert z3["evidence"] == []
    assert "offscreen_materialization_bound" in z3["required_gates"]

    header = HEADER.read_text(encoding="utf-8")
    source = SOURCE.read_text(encoding="utf-8")
    cmake = CMAKE.read_text(encoding="utf-8")
    oracle = ORACLE.read_text(encoding="utf-8")
    assert "candidate_terminal_nodes" in header
    assert "candidate_document_begin" in header
    assert "document_node_count" in header
    assert "CandidateWindowLimitExceeded" in header
    assert "candidate_terminal_nodes.subspan" in source
    assert "for (" not in source
    assert "zevryon-css-style-materialization-window-v1-tests" in cmake
    assert "kDocumentNodes = 100'000U" in oracle
    assert "std::array<std::uint32_t, 5> candidate" in oracle
    assert "stats.selection_work_units == 5U" in oracle


if __name__ == "__main__":
    test_z3_css_materialization_window_foundation_scope()
    print("Z3 CSS materialization window foundation scope passed")
