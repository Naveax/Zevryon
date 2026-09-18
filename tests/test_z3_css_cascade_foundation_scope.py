from __future__ import annotations

import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCOPE = ROOT / "certification" / "z3_css_cascade_foundation_scope.json"
PROGRAM = ROOT / "config" / "zenith_program.json"
HEADER = ROOT / "src" / "css_cascade_v1.hpp"
SOURCE = ROOT / "src" / "css_cascade_v1.cpp"
CMAKE = ROOT / "cmake" / "css_cascade_v1.cmake"
WORKFLOW = ROOT / ".github" / "workflows" / "z3-css-parser-foundation.yml"


def test_z3_css_cascade_foundation_scope() -> None:
    scope = json.loads(SCOPE.read_text(encoding="utf-8"))
    assert scope["schema"] == "zevryon.z3-css-cascade-foundation.v1"
    assert scope["milestone"] == "Z3"
    assert scope["status_change"] is False
    assert scope["gate_progress"] == ["cascade_conformance"]
    assert scope["admitted_gates"] == []
    assert scope["production_surface"] == "cascade_css_author_rules_v1"
    assert scope["resource_class"] == "ComputedStyle"
    assert scope["precedence_order"] == [
        "important",
        "specificity",
        "source-order",
    ]
    assert scope["hard_bounds"] == {
        "maximum_rules_limit": 1_048_576,
        "maximum_declarations_limit": 4_194_304,
        "maximum_properties_limit": 1_048_576,
        "maximum_work_units_limit": 67_108_864,
    }

    provenance = scope["frozen_wpt_provenance"]
    assert provenance["repository"] == "web-platform-tests/wpt"
    assert provenance["commit"] == "15df54d4459b78242d32ae36f9c094a96972bedc"
    assert [(item["path"], item["blob"]) for item in provenance["cases"]] == [
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

    program = json.loads(PROGRAM.read_text(encoding="utf-8"))
    z3 = next(item for item in program["milestones"] if item["id"] == "Z3")
    assert z3["status"] == "planned"
    assert z3["evidence"] == []
    assert "cascade_conformance" in z3["required_gates"]

    header = HEADER.read_text(encoding="utf-8")
    source = SOURCE.read_text(encoding="utf-8")
    cmake = CMAKE.read_text(encoding="utf-8")
    workflow = WORKFLOW.read_text(encoding="utf-8")
    assert "cascade_css_author_rules_v1" in header
    assert "CssCascadeConfigV1" in header
    assert "maximum_work_units" in header
    assert "compare_css_specificity_v1" in source
    assert "WorkBudgetExceeded" in source
    assert "zevryon-css-cascade-v1-tests" in cmake
    assert "zevryon-css-cascade-v1-tests" in workflow


if __name__ == "__main__":
    test_z3_css_cascade_foundation_scope()
    print("Z3 CSS cascade foundation scope contract passed")
