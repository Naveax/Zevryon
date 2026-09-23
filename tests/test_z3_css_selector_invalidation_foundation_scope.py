from __future__ import annotations

import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCOPE = ROOT / "certification" / "z3_css_selector_invalidation_foundation_scope.json"
PROGRAM = ROOT / "config" / "zenith_program.json"
HEADER = ROOT / "src" / "css_selector_invalidation_v1.hpp"
SOURCE = ROOT / "src" / "css_selector_invalidation_v1.cpp"
CMAKE = ROOT / "cmake" / "css_selector_v1.cmake"


def test_z3_css_selector_invalidation_foundation_scope() -> None:
    scope = json.loads(SCOPE.read_text(encoding="utf-8"))
    assert scope["schema"] == "zevryon.z3-css-selector-invalidation-foundation.v1"
    assert scope["milestone"] == "Z3"
    assert scope["status_change"] is False
    assert scope["gate_progress"] == ["selector_dependency_invalidation"]
    assert scope["admitted_gates"] == []
    assert scope["resource_class"] == "ComputedStyle"
    assert scope["hard_bounds"] == {
        "maximum_dependencies_limit": 65_536,
        "maximum_changed_attributes_limit": 65_536,
        "maximum_semantic_bytes_limit": 16_777_216,
        "maximum_work_units_limit": 67_108_864,
    }
    assert (
        "named attribute dependencies own canonical keys in dependency-set PMR storage"
        in scope["dependency_model"]
    )
    assert scope["mutation_model"]["decision"] == (
        "conservative may-affect-match invalidation"
    )

    program = json.loads(PROGRAM.read_text(encoding="utf-8"))
    z3 = next(item for item in program["milestones"] if item["id"] == "Z3")
    assert "selector_dependency_invalidation" in z3["required_gates"]

    header = HEADER.read_text(encoding="utf-8")
    source = SOURCE.read_text(encoding="utf-8")
    cmake = CMAKE.read_text(encoding="utf-8")
    assert "build_css_selector_dependency_set_v1" in header
    assert "css_selector_dependencies_invalidated_v1" in header
    assert "std::pmr::string text" in header
    assert "std::string_view resolve" in header
    assert "maximum_changed_attributes" in header
    assert "ascii_iequals" in source
    assert "append_canonical_name" in source
    assert "WorkBudgetExceeded" in source
    assert "zevryon-css-selector-invalidation-v1-tests" in cmake


if __name__ == "__main__":
    test_z3_css_selector_invalidation_foundation_scope()
    print("Z3 CSS selector invalidation foundation scope contract passed")
