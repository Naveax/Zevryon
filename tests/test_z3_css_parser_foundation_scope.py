from __future__ import annotations

import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROGRAM = ROOT / "config" / "zenith_program.json"
SCOPE = ROOT / "certification" / "z3_css_parser_foundation_scope.json"


def test_z3_css_parser_foundation_scope_is_exact() -> None:
    program = json.loads(PROGRAM.read_text(encoding="utf-8"))
    scope = json.loads(SCOPE.read_text(encoding="utf-8"))

    z3 = next(item for item in program["milestones"] if item["id"] == "Z3")
    assert z3["dependencies"] == ["Z0", "Z8"]
    assert z3["required_gates"] == [
        "css_parser_conformance",
        "cascade_conformance",
        "selector_dependency_invalidation",
        "bounded_style_dag",
        "offscreen_materialization_bound",
    ]

    assert scope["schema"] == "zevryon.z3-css-parser-foundation-scope.v1"
    assert scope["milestone"] == "Z3"
    assert scope["status_change"] is False
    assert scope["gate_progress"] == ["css_parser_conformance"]
    assert scope["admitted_gates"] == []
    assert scope["production_surface"] == "parse_css_stylesheet_v1 in zevryon-massivedoc-core"
    assert scope["resource_class"] == "ComputedStyle"
    assert scope["configuration_hard_maxima"] == {
        "input_bytes": 16_777_216,
        "rules": 1_048_576,
        "declarations": 4_194_304,
        "nesting_depth": 256,
        "output_text_bytes": 134_217_728,
    }
    exclusions = set(scope["deliberate_exclusions"])
    assert "at-rules" in exclusions
    assert "cascade and specificity" in exclusions
    assert "full CSS Syntax or WPT conformance claim" in exclusions
    assert "css_parser_conformance" not in scope["admitted_gates"]


if __name__ == "__main__":
    test_z3_css_parser_foundation_scope_is_exact()
    print("Z3 CSS parser foundation scope contract passed")
