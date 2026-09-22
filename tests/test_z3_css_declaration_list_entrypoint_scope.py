from __future__ import annotations

import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCOPE = ROOT / "certification" / "z3_css_declaration_list_entrypoint_scope.json"
PROGRAM = ROOT / "config" / "zenith_program.json"
HEADER = ROOT / "src" / "css_parser_v1.hpp"
SOURCE = ROOT / "src" / "css_parser_v1.cpp"
CMAKE = ROOT / "cmake" / "css_parser_v1.cmake"
ORACLE = ROOT / "tests" / "css_declaration_list_v1_tests.cpp"


def test_z3_css_declaration_list_entrypoint_scope() -> None:
    scope = json.loads(SCOPE.read_text(encoding="utf-8"))
    assert (
        scope["schema"]
        == "zevryon.z3-css-declaration-list-entrypoint-foundation.v1"
    )
    assert scope["milestone"] == "Z3"
    assert scope["status_change"] is False
    assert scope["admitted_gates"] == []
    assert scope["production_surface"] == "parse_css_declaration_list_v1"
    assert (
        "one internal closing-brace sentinel is appended after preprocessing and is excluded from input and preprocessing statistics"
        in scope["parser_reuse_contract"]
    )
    assert (
        "successful standalone output contains no synthetic qualified rule"
        in scope["parser_reuse_contract"]
    )

    program = json.loads(PROGRAM.read_text(encoding="utf-8"))
    z3 = next(item for item in program["milestones"] if item["id"] == "Z3")
    assert z3["status"] == "planned"
    assert z3["evidence"] == []

    header = HEADER.read_text(encoding="utf-8")
    source = SOURCE.read_text(encoding="utf-8")
    cmake = CMAKE.read_text(encoding="utf-8")
    oracle = ORACLE.read_text(encoding="utf-8")

    assert "parse_css_declaration_list_v1" in header
    assert "bool run_declaration_list()" in source
    assert "preprocessed.push_back('}')" in source
    assert "parser.run_declaration_list()" in source
    assert "unexpected closing brace" in source
    assert "zevryon-css-declaration-list-v1-tests" in cmake
    assert "owner_rule_index == kCssAtRuleNoOwnerV1" in oracle
    assert "output.rules.empty()" in oracle


if __name__ == "__main__":
    test_z3_css_declaration_list_entrypoint_scope()
    print("Z3 CSS declaration-list entrypoint scope passed")
