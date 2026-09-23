from __future__ import annotations

import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCOPE = ROOT / "certification" / "z3_css_declaration_list_scope.json"
PROGRAM = ROOT / "config" / "zenith_program.json"
ORACLE = ROOT / "tests" / "css_parser_v1_tests.cpp"


def test_z3_css_declaration_list_scope() -> None:
    scope = json.loads(SCOPE.read_text(encoding="utf-8"))
    assert scope["schema"] == "zevryon.z3-css-declaration-list-authority.v1"
    assert scope["milestone"] == "Z3"
    assert scope["status_change"] is False
    assert scope["gate_progress"] == ["css_parser_conformance"]
    assert scope["admitted_gates"] == []
    assert scope["resource_class"] == "ComputedStyle"

    wpt = scope["wpt_reference"]
    assert wpt["repository"] == "web-platform-tests/wpt"
    assert wpt["upstream_commit"] == "15df54d4459b78242d32ae36f9c094a96972bedc"
    files = {entry["path"]: entry for entry in wpt["files"]}
    assert files["css/css-syntax/declarations-trim-whitespace.html"] == {
        "path": "css/css-syntax/declarations-trim-whitespace.html",
        "blob_sha": "a7d69d149e7bbf854efc2ef19af132af372ff521",
        "admitted_cases": 9,
    }
    assert files["css/css-syntax/missing-semicolon.html"] == {
        "path": "css/css-syntax/missing-semicolon.html",
        "blob_sha": "d8e70e631591c05d2025d1800b1201ce80550fed",
        "admitted_cases": 1,
    }
    assert files["css/css-syntax/support/missing-semicolon.css"] == {
        "path": "css/css-syntax/support/missing-semicolon.css",
        "blob_sha": "0d9a0bbda757b8b4197d6e060f08657d12cf9e93",
        "fixture_for": "missing-semicolon.html",
    }
    assert scope["authority_cases"] == {
        "whitespace_and_important": 9,
        "missing_final_semicolon": 1,
    }

    program = json.loads(PROGRAM.read_text(encoding="utf-8"))
    z3 = next(item for item in program["milestones"] if item["id"] == "Z3")
    assert "css_parser_conformance" in z3["required_gates"]

    oracle = ORACLE.read_text(encoding="utf-8")
    assert "test_wpt_declaration_list_authority" in oracle
    assert "--foo-9:bar" in oracle
    assert "color: green" in oracle


if __name__ == "__main__":
    test_z3_css_declaration_list_scope()
    print("Z3 CSS declaration-list scope contract passed")
