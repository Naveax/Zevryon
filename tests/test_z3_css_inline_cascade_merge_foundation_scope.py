from __future__ import annotations

import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCOPE = ROOT / "certification" / "z3_css_inline_cascade_merge_foundation_scope.json"
PROGRAM = ROOT / "config" / "zenith_program.json"
HEADER = ROOT / "src" / "css_inline_cascade_merge_v1.hpp"
SOURCE = ROOT / "src" / "css_inline_cascade_merge_v1.cpp"
CMAKE = ROOT / "cmake" / "css_semantic_style_bridge_v1.cmake"
ORACLE = ROOT / "tests" / "css_inline_cascade_merge_v1_tests.cpp"


def test_z3_css_inline_cascade_merge_foundation_scope() -> None:
    scope = json.loads(SCOPE.read_text(encoding="utf-8"))
    assert scope["schema"] == "zevryon.z3-css-inline-cascade-merge-foundation.v1"
    assert scope["milestone"] == "Z3"
    assert scope["status_change"] is False
    assert scope["admitted_gates"] == []
    assert scope["hard_bounds"] == {
        "maximum_author_properties_limit": 1_048_576,
        "maximum_inline_declarations_limit": 1_048_576,
        "maximum_output_properties_limit": 1_048_576,
        "maximum_output_text_bytes_limit": 134_217_728,
        "maximum_work_units_limit": 67_108_864,
    }
    assert (
        "merged output provenance fields are not cascade authority and exist only to feed the style DAG"
        in scope["precedence_contract"]
    )

    program = json.loads(PROGRAM.read_text(encoding="utf-8"))
    z3 = next(item for item in program["milestones"] if item["id"] == "Z3")
    assert z3["status"] == "planned"
    assert z3["evidence"] == []

    header = HEADER.read_text(encoding="utf-8")
    source = SOURCE.read_text(encoding="utf-8")
    cmake = CMAKE.read_text(encoding="utf-8")
    oracle = ORACLE.read_text(encoding="utf-8")

    assert "merge_css_author_and_inline_cascade_v1" in header
    assert "std::span<const CssDeclarationV1>" in header
    assert "current.inline_origin" in source
    assert "author_important_preserved" in source
    assert "inline_overrides_author" in source
    assert "intern_css_cascade_style_v1" in oracle
    assert "custom property identity must remain case-sensitive" in oracle
    assert "zevryon-css-inline-cascade-merge-v1-tests" in cmake


if __name__ == "__main__":
    test_z3_css_inline_cascade_merge_foundation_scope()
    print("Z3 CSS inline cascade merge foundation scope passed")
