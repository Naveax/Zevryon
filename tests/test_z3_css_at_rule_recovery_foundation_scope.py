from __future__ import annotations

import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCOPE = ROOT / "certification" / "z3_css_at_rule_recovery_foundation_scope.json"
PROGRAM = ROOT / "config" / "zenith_program.json"
HEADER = ROOT / "src" / "css_parser_v1.hpp"
SOURCE = ROOT / "src" / "css_parser_v1.cpp"
CMAKE = ROOT / "cmake" / "css_parser_v1.cmake"


def test_z3_css_at_rule_recovery_foundation_scope() -> None:
    scope = json.loads(SCOPE.read_text(encoding="utf-8"))
    assert scope["schema"] == "zevryon.z3-css-at-rule-recovery-foundation.v1"
    assert scope["milestone"] == "Z3"
    assert scope["status_change"] is False
    assert scope["gate_progress"] == ["css_parser_conformance"]
    assert scope["admitted_gates"] == []
    assert scope["resource_class"] == "ComputedStyle"
    assert scope["configuration_hard_maxima"] == {
        "at_rules": 1_048_576,
        "nesting_depth": 256,
    }
    assert scope["authority_cases"] == {
        "wpt_cases": 3,
        "local_recovery_cases": 5,
        "local_at_rule_boundary_cases": 3,
        "local_owner_identity_cases": 1,
    }

    wpt = scope["wpt_reference"]
    assert wpt["repository"] == "web-platform-tests/wpt"
    assert wpt["upstream_commit"] == "15df54d4459b78242d32ae36f9c094a96972bedc"
    assert [(item["path"], item["blob_sha"], item["admitted_cases"]) for item in wpt["files"]] == [
        (
            "css/css-syntax/charset-is-not-a-rule.html",
            "ff8d3298b6216f7cedb29939554a30c65eac4bf0",
            1,
        ),
        (
            "css/css-syntax/at-rule-in-declaration-list.html",
            "f40975d27eadf436634491b4a158cee4f9c268ef",
            2,
        ),
    ]

    program = json.loads(PROGRAM.read_text(encoding="utf-8"))
    z3 = next(item for item in program["milestones"] if item["id"] == "Z3")
    assert z3["status"] == "planned"
    assert z3["evidence"] == []
    assert "css_parser_conformance" in z3["required_gates"]

    header = HEADER.read_text(encoding="utf-8")
    source = SOURCE.read_text(encoding="utf-8")
    cmake = CMAKE.read_text(encoding="utf-8")
    assert "CssAtRuleV1" in header
    assert "owner_rule_index" in header
    assert "maximum_at_rules" in header
    assert "recovered_invalid_declarations" in header
    assert "parse_at_rule(CssAtRuleContextV1 context)" in source
    assert "recover_bad_declaration" in source
    assert "dropped_charset_rules" in source
    assert "zevryon-css-parser-at-rule-recovery-v1-tests" in cmake


if __name__ == "__main__":
    test_z3_css_at_rule_recovery_foundation_scope()
    print("Z3 CSS at-rule/recovery foundation scope contract passed")
