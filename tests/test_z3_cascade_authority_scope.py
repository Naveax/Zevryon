from __future__ import annotations

import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCOPE = ROOT / "certification" / "z3_cascade_authority_scope.json"
PROGRAM = ROOT / "config" / "zenith_program.json"
TEST = ROOT / "tests" / "z3_cascade_authority_tests.cpp"
CMAKE = ROOT / "cmake" / "css_cascade_v1.cmake"


def test_z3_cascade_authority_scope() -> None:
    scope = json.loads(SCOPE.read_text(encoding="utf-8"))
    assert scope["schema"] == "zevryon.z3-cascade-authority-scope.v1"
    assert scope["milestone"] == "Z3"
    assert scope["status_change"] is False
    assert scope["admitted_gates"] == ["cascade_conformance"]
    assert scope["remaining_gates"] == [
        "css_parser_conformance",
        "selector_dependency_invalidation",
        "bounded_style_dag",
        "offscreen_materialization_bound",
    ]
    assert scope["authority_denominator"] == {
        "specificity_profiles": 4,
        "importance_states_per_side": 2,
        "exact_winner_decisions": 64,
        "importance_replacements": 16,
        "specificity_replacements": 12,
        "source_order_replacements": 8,
        "bound_failure_kinds": [
            "RuleLimitExceeded",
            "DeclarationLimitExceeded",
            "PropertyLimitExceeded",
            "WorkBudgetExceeded",
        ],
    }
    assert scope["required_platforms"] == ["ubuntu-24.04", "windows-2022"]
    assert scope["configured_author_origin_profile_claim"] is True
    assert scope["user_origin_claim"] is False
    assert scope["user_agent_origin_claim"] is False
    assert scope["cascade_layers_claim"] is False
    assert scope["inline_style_origin_claim"] is False
    assert scope["inheritance_claim"] is False
    assert scope["shorthand_claim"] is False

    provenance = scope["frozen_wpt_provenance"]
    assert provenance["repository"] == "web-platform-tests/wpt"
    assert provenance["commit"] == "15df54d4459b78242d32ae36f9c094a96972bedc"
    assert [(case["path"], case["blob"]) for case in provenance["cases"]] == [
        (
            "css/CSS2/cascade/specificity-001.xht",
            "d0044ad1b4784e69e1c84b97752e5f7794798dbc",
        ),
        (
            "css/CSS2/cascade/specificity-007.xht",
            "3d4bd830c5f40974221533a20627ab0fa417ff4e",
        ),
        (
            "css/CSS2/cascade/cascade-005.xht",
            "e6011975c90bfb7601482457d101c083fd442262",
        ),
    ]

    test = TEST.read_text(encoding="utf-8")
    cmake = CMAKE.read_text(encoding="utf-8")
    assert "kDecisionDenominator" in test
    assert "importance_replacements == 16U" in test
    assert "specificity_replacements == 12U" in test
    assert "source_order_replacements == 8U" in test
    assert "RuleLimitExceeded" in test
    assert "DeclarationLimitExceeded" in test
    assert "PropertyLimitExceeded" in test
    assert "WorkBudgetExceeded" in test
    assert "zevryon-z3-cascade-authority-tests" in cmake

    program = json.loads(PROGRAM.read_text(encoding="utf-8"))
    z3 = next(item for item in program["milestones"] if item["id"] == "Z3")
    assert z3["status"] == "planned"
    assert z3["evidence"] == []
    assert "cascade_conformance" in z3["required_gates"]


if __name__ == "__main__":
    test_z3_cascade_authority_scope()
    print("Z3 cascade authority scope contract passed")
