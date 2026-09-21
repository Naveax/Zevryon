from __future__ import annotations

import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCOPE = ROOT / "certification" / "z3_bounded_style_dag_authority_scope.json"
PROGRAM = ROOT / "config" / "zenith_program.json"
CMAKE = ROOT / "cmake" / "css_style_dag_v1.cmake"
AUTHORITY = ROOT / "tests" / "css_style_dag_authority_v1_tests.cpp"


def test_z3_bounded_style_dag_authority_scope() -> None:
    scope = json.loads(SCOPE.read_text(encoding="utf-8"))
    assert scope["schema"] == "zevryon.z3-bounded-style-dag-authority-scope.v1"
    assert scope["milestone"] == "Z3"
    assert scope["status_change"] is False
    assert scope["admitted_gates"] == ["bounded_style_dag"]
    assert scope["remaining_gates"] == [
        "css_parser_conformance",
        "cascade_conformance",
        "selector_dependency_invalidation",
        "offscreen_materialization_bound",
    ]
    assert scope["production_surface"] == "intern_css_cascade_style_v1"
    assert scope["resource_class"] == "ComputedStyle"
    assert scope["authority_denominator"] == {
        "foundation_oracle_cases": 5,
        "fanout_styles": 512,
        "provenance_replays": 512,
        "expected_retained_nodes": 515,
        "expected_retained_text_bytes": 3880,
        "boundary_authority_cases": 7,
    }
    assert scope["required_platforms"] == [
        "ubuntu-24.04",
        "windows-2022",
    ]
    assert scope["full_css_computed_style_claim"] is False
    assert scope["inheritance_claim"] is False
    assert scope["computed_value_normalization_claim"] is False

    program = json.loads(PROGRAM.read_text(encoding="utf-8"))
    z3 = next(item for item in program["milestones"] if item["id"] == "Z3")
    assert z3["status"] == "planned"
    assert z3["evidence"] == []
    assert "bounded_style_dag" in z3["required_gates"]

    cmake = CMAKE.read_text(encoding="utf-8")
    authority = AUTHORITY.read_text(encoding="utf-8")
    assert "zevryon-css-style-dag-authority-v1-tests" in cmake
    assert "kFanoutStyles = 512U" in authority
    assert "kExpectedRetainedNodes = 515U" in authority
    assert "kExpectedRetainedTextBytes = 3880U" in authority
    assert "provenance replay must add no DAG storage" in authority
    assert "NodeLimitExceeded" in authority
    assert "TextBudgetExceeded" in authority
    assert "SemanticBudgetExceeded" in authority
    assert "WorkBudgetExceeded" in authority
    assert "CorruptDag" in authority
    assert "AllocationFailure" in authority


if __name__ == "__main__":
    test_z3_bounded_style_dag_authority_scope()
    print("Z3 bounded style DAG authority scope passed")
