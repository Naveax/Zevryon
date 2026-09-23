from __future__ import annotations

import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCOPE = ROOT / "certification" / "z3_css_style_dag_foundation_scope.json"
PROGRAM = ROOT / "config" / "zenith_program.json"
HEADER = ROOT / "src" / "css_style_dag_v1.hpp"
SOURCE = ROOT / "src" / "css_style_dag_v1.cpp"
CMAKE = ROOT / "cmake" / "css_style_dag_v1.cmake"


def test_z3_css_style_dag_foundation_scope() -> None:
    scope = json.loads(SCOPE.read_text(encoding="utf-8"))
    assert scope["schema"] == "zevryon.z3-css-style-dag-foundation.v1"
    assert scope["milestone"] == "Z3"
    assert scope["status_change"] is False
    assert scope["gate_progress"] == ["bounded_style_dag"]
    assert scope["admitted_gates"] == []
    assert scope["resource_class"] == "ComputedStyle"
    assert scope["hard_bounds"] == {
        "maximum_properties_per_style_limit": 65_536,
        "maximum_nodes_limit": 1_048_576,
        "maximum_text_bytes_limit": 134_217_728,
        "maximum_style_semantic_bytes_limit": 16_777_216,
        "maximum_work_units_limit": 67_108_864,
    }
    assert "cascade provenance is excluded after winner selection" in scope[
        "identity_model"
    ]
    assert "equal complete computed styles return the same terminal node" in scope[
        "sharing_model"
    ]

    program = json.loads(PROGRAM.read_text(encoding="utf-8"))
    z3 = next(item for item in program["milestones"] if item["id"] == "Z3")
    assert "bounded_style_dag" in z3["required_gates"]

    header = HEADER.read_text(encoding="utf-8")
    source = SOURCE.read_text(encoding="utf-8")
    cmake = CMAKE.read_text(encoding="utf-8")
    assert "intern_css_cascade_style_v1" in header
    assert "CssComputedStyleDagV1" in header
    assert "maximum_style_semantic_bytes" in header
    assert "validate_existing_dag" in source
    assert "retained_validation_comparisons" in source
    assert "canonical_order_comparisons" in source
    assert "child_lookup_nodes_scanned" in source
    assert "child_lookup_comparisons" in source
    assert "zevryon-css-style-dag-v1-tests" in cmake


if __name__ == "__main__":
    test_z3_css_style_dag_foundation_scope()
    print("Z3 CSS style DAG foundation scope contract passed")
