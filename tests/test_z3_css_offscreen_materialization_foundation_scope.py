from __future__ import annotations

import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCOPE = ROOT / "certification" / "z3_css_offscreen_materialization_foundation_scope.json"
PROGRAM = ROOT / "config" / "zenith_program.json"
HEADER = ROOT / "src" / "css_style_materialization_v1.hpp"
SOURCE = ROOT / "src" / "css_style_materialization_v1.cpp"
CMAKE = ROOT / "cmake" / "css_style_materialization_v1.cmake"


def test_z3_css_offscreen_materialization_foundation_scope() -> None:
    scope = json.loads(SCOPE.read_text(encoding="utf-8"))
    assert scope["schema"] == "zevryon.z3-css-offscreen-materialization-foundation.v1"
    assert scope["milestone"] == "Z3"
    assert scope["status_change"] is False
    assert scope["gate_progress"] == ["offscreen_materialization_bound"]
    assert scope["admitted_gates"] == []
    assert scope["resource_class"] == "ComputedStyle"
    assert scope["hard_bounds"] == {
        "maximum_requests_limit": 65_536,
        "maximum_unique_styles_limit": 65_536,
        "maximum_properties_limit": 1_048_576,
        "maximum_text_bytes_limit": 134_217_728,
        "maximum_properties_per_style_limit": 65_536,
        "maximum_work_units_limit": 67_108_864,
    }
    assert "duplicate requests map to the same materialized style record" in scope[
        "request_model"
    ]
    assert "materialized output remains valid after the source style DAG is released" in scope[
        "ownership_model"
    ]

    program = json.loads(PROGRAM.read_text(encoding="utf-8"))
    z3 = next(item for item in program["milestones"] if item["id"] == "Z3")
    assert z3["status"] == "planned"
    assert z3["evidence"] == []
    assert "offscreen_materialization_bound" in z3["required_gates"]

    header = HEADER.read_text(encoding="utf-8")
    source = SOURCE.read_text(encoding="utf-8")
    cmake = CMAKE.read_text(encoding="utf-8")
    assert "materialize_css_style_nodes_v1" in header
    assert "CssStyleMaterializationBatchV1" in header
    assert "maximum_unique_styles" in header
    assert "request_style_indices" in header
    assert "validate_chain_node" in source
    assert "duplicate_lookup_comparisons" in source
    assert "zevryon-css-style-materialization-v1-tests" in cmake


if __name__ == "__main__":
    test_z3_css_offscreen_materialization_foundation_scope()
    print("Z3 CSS offscreen materialization foundation scope contract passed")
