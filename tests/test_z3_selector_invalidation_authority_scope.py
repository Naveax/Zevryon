from __future__ import annotations

import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCOPE = ROOT / "certification" / "z3_selector_invalidation_authority_scope.json"
PROGRAM = ROOT / "config" / "zenith_program.json"
TEST = ROOT / "tests" / "z3_selector_invalidation_authority_tests.cpp"
CMAKE = ROOT / "cmake" / "css_selector_v1.cmake"


def test_z3_selector_invalidation_authority_scope() -> None:
    scope = json.loads(SCOPE.read_text(encoding="utf-8"))
    assert scope["schema"] == "zevryon.z3-selector-invalidation-authority-scope.v1"
    assert scope["milestone"] == "Z3"
    assert scope["status_change"] is False
    assert scope["admitted_gates"] == ["selector_dependency_invalidation"]
    assert scope["remaining_gates"] == [
        "css_parser_conformance",
        "cascade_conformance",
        "bounded_style_dag",
        "offscreen_materialization_bound",
    ]
    assert scope["authority_denominator"] == {
        "selector_profiles": 32,
        "mutations_per_profile": 8,
        "exact_invalidation_decisions": 256,
        "dependency_dimensions": [
            "tag",
            "id",
            "class",
            "data-a",
            "data-b",
        ],
        "deduplication_case_collapsed_dependencies": 3,
    }
    assert scope["required_platforms"] == ["ubuntu-24.04", "windows-2022"]
    assert scope["supported_selector_profile_claim"] is True
    assert scope["combinator_dependency_claim"] is False
    assert scope["pseudo_class_dependency_claim"] is False
    assert scope["ancestor_sibling_propagation_claim"] is False

    program = json.loads(PROGRAM.read_text(encoding="utf-8"))
    z3 = next(item for item in program["milestones"] if item["id"] == "Z3")
    assert "selector_dependency_invalidation" in z3["required_gates"]

    test = TEST.read_text(encoding="utf-8")
    cmake = CMAKE.read_text(encoding="utf-8")
    assert "kSelectorProfiles = 32U" in test
    assert "kMutationsPerProfile = 8U" in test
    assert "kDecisionDenominator" in test
    assert "std::popcount(mask)" in test
    assert "DependencyLimitExceeded" in test
    assert "WorkBudgetExceeded" in test
    assert "zevryon-z3-selector-invalidation-authority-tests" in cmake


if __name__ == "__main__":
    test_z3_selector_invalidation_authority_scope()
    print("Z3 selector invalidation authority scope contract passed")
