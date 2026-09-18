from __future__ import annotations

import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCOPE = ROOT / "certification" / "z3_css_selector_foundation_scope.json"
PROGRAM = ROOT / "config" / "zenith_program.json"
HEADER = ROOT / "src" / "css_selector_v1.hpp"
SOURCE = ROOT / "src" / "css_selector_v1.cpp"
CMAKE = ROOT / "cmake" / "css_selector_v1.cmake"


def test_z3_css_selector_foundation_scope() -> None:
    scope = json.loads(SCOPE.read_text(encoding="utf-8"))
    assert scope["schema"] == "zevryon.z3-css-selector-foundation.v1"
    assert scope["milestone"] == "Z3"
    assert scope["status_change"] is False
    assert scope["admitted_gates"] == []
    assert scope["gate_progress"] == [
        "cascade_conformance",
        "selector_dependency_invalidation",
    ]
    assert scope["resource_class"] == "ComputedStyle"
    assert scope["supported_simple_selectors"] == [
        "universal",
        "type",
        "id",
        "class",
        "attribute-exists",
        "attribute-equals",
    ]
    assert scope["hard_bounds"] == {
        "maximum_input_bytes_limit": 1048576,
        "maximum_simple_selectors_limit": 65536,
        "maximum_output_text_bytes_limit": 4194304,
        "maximum_match_attributes_limit": 65536,
        "maximum_match_semantic_bytes_limit": 16777216,
        "maximum_match_work_units_limit": 67108864,
    }

    program = json.loads(PROGRAM.read_text(encoding="utf-8"))
    z3 = next(item for item in program["milestones"] if item["id"] == "Z3")
    assert z3["status"] == "planned"
    assert z3["evidence"] == []
    assert "cascade_conformance" in z3["required_gates"]
    assert "selector_dependency_invalidation" in z3["required_gates"]

    header = HEADER.read_text(encoding="utf-8")
    source = SOURCE.read_text(encoding="utf-8")
    cmake = CMAKE.read_text(encoding="utf-8")
    assert "compile_css_compound_selector_v1" in header
    assert "match_css_compound_selector_v1" in header
    assert "compare_css_specificity_v1" in header
    assert "CssSelectorSimpleKindV1" in header
    assert "CssSelectorMatchConfigV1" in header
    assert "maximum_work_units" in header
    assert "class_token_matches" in source
    assert "zevryon-css-selector-v1-tests" in cmake


if __name__ == "__main__":
    test_z3_css_selector_foundation_scope()
    print("Z3 CSS selector foundation scope contract passed")
