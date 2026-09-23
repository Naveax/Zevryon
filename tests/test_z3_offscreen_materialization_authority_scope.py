from __future__ import annotations

import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCOPE = ROOT / "certification" / "z3_offscreen_materialization_authority_scope.json"
PROGRAM = ROOT / "config" / "zenith_program.json"
CMAKE = ROOT / "cmake" / "css_style_materialization_window_v1.cmake"
AUTHORITY = ROOT / "tests" / "css_offscreen_materialization_authority_v1_tests.cpp"
WINDOW_HEADER = ROOT / "src" / "css_style_materialization_window_v1.hpp"
WINDOW_SOURCE = ROOT / "src" / "css_style_materialization_window_v1.cpp"


def test_z3_offscreen_materialization_authority_scope() -> None:
    scope = json.loads(SCOPE.read_text(encoding="utf-8"))
    assert scope["schema"] == "zevryon.z3-offscreen-materialization-authority-scope.v1"
    assert scope["milestone"] == "Z3"
    assert scope["status_change"] is False
    assert scope["admitted_gates"] == ["offscreen_materialization_bound"]
    assert scope["remaining_gates"] == [
        "css_parser_conformance",
        "cascade_conformance",
        "selector_dependency_invalidation",
        "bounded_style_dag",
    ]
    assert scope["resource_class"] == "ComputedStyle"
    assert scope["authority_denominator"] == {
        "document_nodes": 1_000_000,
        "candidate_terminal_nodes": 1_536,
        "candidate_terminal_bytes": 6_144,
        "visible_nodes": 1_024,
        "offscreen_before_nodes": 256,
        "offscreen_after_nodes": 256,
        "selected_requests": 1_536,
        "unique_styles": 64,
        "duplicate_requests": 1_472,
        "materialized_properties": 128,
        "materialized_text_bytes": 492,
        "retained_dag_nodes": 66,
        "retained_dag_text_bytes": 366,
        "materializer_foundation_cases": 6,
        "window_foundation_cases": 6,
    }
    assert scope["required_platforms"] == [
        "ubuntu-24.04",
        "windows-2022",
    ]
    assert scope["z8_bounded_projection_shape_compatible"] is True
    assert scope["automatic_dom_to_terminal_discovery_claim"] is False
    assert scope["pixel_viewport_geometry_claim"] is False
    assert scope["layout_materialization_claim"] is False
    assert scope["paint_materialization_claim"] is False

    program = json.loads(PROGRAM.read_text(encoding="utf-8"))
    z3 = next(item for item in program["milestones"] if item["id"] == "Z3")
    assert "offscreen_materialization_bound" in z3["required_gates"]

    cmake = CMAKE.read_text(encoding="utf-8")
    authority = AUTHORITY.read_text(encoding="utf-8")
    header = WINDOW_HEADER.read_text(encoding="utf-8")
    source = WINDOW_SOURCE.read_text(encoding="utf-8")
    assert "zevryon-css-offscreen-materialization-authority-v1-tests" in cmake
    assert "kDocumentNodes = 1'000'000U" in authority
    assert "std::array<std::uint32_t, kSelectedRequests> candidate" in authority
    assert "kCandidateTerminalBytes == 6'144U" in authority
    assert "std::vector<std::uint32_t> document" not in authority
    assert "candidate_terminal_nodes" in header
    assert "candidate_document_begin" in header
    assert "candidate_terminal_nodes.subspan" in source
    assert "for (" not in source


if __name__ == "__main__":
    test_z3_offscreen_materialization_authority_scope()
    print("Z3 offscreen materialization authority scope passed")
