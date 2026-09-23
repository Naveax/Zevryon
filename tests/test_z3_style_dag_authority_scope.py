from __future__ import annotations

import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCOPE = ROOT / "certification" / "z3_style_dag_authority_scope.json"
PROGRAM = ROOT / "config" / "zenith_program.json"
CMAKE = ROOT / "cmake" / "css_style_dag_v1.cmake"
TEST = ROOT / "tests" / "z3_style_dag_authority_tests.cpp"


def test_z3_style_dag_authority_scope() -> None:
    scope = json.loads(SCOPE.read_text(encoding="utf-8"))
    assert scope["schema"] == "zevryon.z3-style-dag-authority-scope.v1"
    assert scope["milestone"] == "Z3"
    assert scope["status_change"] is False
    assert scope["admitted_gates"] == ["bounded_style_dag"]
    assert scope["remaining_gates"] == [
        "css_parser_conformance",
        "cascade_conformance",
        "selector_dependency_invalidation",
        "offscreen_materialization_bound",
    ]
    assert scope["production_path"] == (
        "parse_css_stylesheet_v1 -> cascade_css_author_rules_v1 -> "
        "intern_css_cascade_style_v1"
    )
    assert scope["authority_fixture"] == {
        "unique_styles": 32,
        "properties_per_style": 8,
        "total_intern_requests": 4096,
        "shared_prefix_properties": 7,
        "expected_retained_nodes": 40,
        "node_limit_rejection_ceiling": 39,
        "computed_style_ledger_hard_limit_bytes": 1_048_576,
    }
    assert scope["platforms"] == ["ubuntu-24.04", "windows-2022"]
    assert scope["full_computed_style_semantics_claim"] is False

    program = json.loads(PROGRAM.read_text(encoding="utf-8"))
    z3 = next(item for item in program["milestones"] if item["id"] == "Z3")
    assert z3["required_gates"] == [
        "css_parser_conformance",
        "cascade_conformance",
        "selector_dependency_invalidation",
        "bounded_style_dag",
        "offscreen_materialization_bound",
    ]

    cmake = CMAKE.read_text(encoding="utf-8")
    test = TEST.read_text(encoding="utf-8")
    assert "zevryon-z3-style-dag-authority-tests" in cmake
    assert "kUniqueStyles = 32U" in test
    assert "kPropertiesPerStyle = 8U" in test
    assert "kTotalRequests = 4096U" in test
    assert "kExpectedNodes" in test
    assert "kDagLedgerHardLimit" in test
    assert "NodeLimitExceeded" in test


if __name__ == "__main__":
    test_z3_style_dag_authority_scope()
    print("Z3 style DAG authority scope contract passed")
