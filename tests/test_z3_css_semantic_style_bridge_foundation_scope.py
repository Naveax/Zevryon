from __future__ import annotations

import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCOPE = ROOT / "certification" / "z3_css_semantic_style_bridge_foundation_scope.json"
PROGRAM = ROOT / "config" / "zenith_program.json"
HEADER = ROOT / "src" / "css_semantic_style_bridge_v1.hpp"
SOURCE = ROOT / "src" / "css_semantic_style_bridge_v1.cpp"
CMAKE = ROOT / "cmake" / "css_semantic_style_bridge_v1.cmake"
PARENT_CMAKE = ROOT / "cmake" / "css_style_materialization_v1.cmake"
ORACLE = ROOT / "tests" / "css_semantic_style_bridge_v1_tests.cpp"


def test_z3_css_semantic_style_bridge_foundation_scope() -> None:
    scope = json.loads(SCOPE.read_text(encoding="utf-8"))
    assert scope["schema"] == "zevryon.z3-css-semantic-style-bridge-foundation.v1"
    assert scope["milestone"] == "Z3"
    assert scope["status_change"] is False
    assert scope["admitted_gates"] == []
    assert scope["store_facing_input"] == "ZenithSemanticNodeWindowResult"
    assert scope["hard_bounds"] == {
        "maximum_nodes_limit": 65_536,
        "maximum_attributes_per_node_limit": 65_536,
        "maximum_total_attributes_limit": 1_048_576,
        "maximum_node_semantic_bytes_limit": 16_777_216,
        "maximum_total_semantic_bytes_limit": 268_435_456,
        "maximum_work_units_limit": 134_217_728,
    }
    assert (
        "non-empty HTML style attribute fails before any DAG mutation because inline-origin cascade is not yet authoritative"
        in scope["certified_foundation"]
    )

    program = json.loads(PROGRAM.read_text(encoding="utf-8"))
    z3 = next(item for item in program["milestones"] if item["id"] == "Z3")
    assert z3["status"] == "planned"
    assert z3["evidence"] == []

    header = HEADER.read_text(encoding="utf-8")
    source = SOURCE.read_text(encoding="utf-8")
    cmake = CMAKE.read_text(encoding="utf-8")
    parent_cmake = PARENT_CMAKE.read_text(encoding="utf-8")
    oracle = ORACLE.read_text(encoding="utf-8")

    assert "compute_css_style_terminals_for_semantic_window_v1" in header
    assert "ZenithSemanticNodeWindowResult" in header
    assert "InlineStyleUnsupported" in header
    assert "cascade_css_author_rules_v1" in source
    assert "intern_css_cascade_style_v1" in source
    assert "node.style.empty()" in source
    assert "maximum_work_units - candidate_stats.work_units" in source
    assert "zevryon-css-semantic-style-bridge-v1-tests" in cmake
    assert "css_semantic_style_bridge_v1.cmake" in parent_cmake
    assert "arena_node_count = 1'000'000U" in oracle
    assert "output.terminal_nodes[0] ==" in oracle


if __name__ == "__main__":
    test_z3_css_semantic_style_bridge_foundation_scope()
    print("Z3 CSS semantic style bridge foundation scope passed")
