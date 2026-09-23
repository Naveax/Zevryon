from __future__ import annotations

import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCOPE = ROOT / "certification" / "z3_css_parser_authority_scope.json"
PROGRAM = ROOT / "config" / "zenith_program.json"
MANIFEST = ROOT / "config" / "z3_css_parser_corpus_v1.json"
RUNNER = ROOT / "scripts" / "z3_css_parser_runner_v1.py"
PROBE = ROOT / "tests" / "css_parser_v1_probe.cpp"
CMAKE = ROOT / "cmake" / "css_parser_v1.cmake"


def test_z3_css_parser_authority_scope() -> None:
    scope = json.loads(SCOPE.read_text(encoding="utf-8"))
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    runner = RUNNER.read_text(encoding="utf-8")
    probe = PROBE.read_text(encoding="utf-8")
    cmake = CMAKE.read_text(encoding="utf-8")

    assert scope["schema"] == "zevryon.z3-css-parser-authority-scope.v1"
    assert scope["milestone"] == "Z3"
    assert scope["status_change"] is False
    assert scope["admitted_gates"] == ["css_parser_conformance"]
    assert scope["authority"] == "z3-css-parser-conformance-v1"
    assert scope["corpus"] == {
        "manifest": "config/z3_css_parser_corpus_v1.json",
        "configured_cases": 33,
        "success_cases": 27,
        "error_cases": 6,
        "wpt_provenance_cases": 18,
        "local_boundary_cases": 15,
        "required_passed": 33,
        "required_failed": 0,
        "required_unsupported": 0,
    }
    assert scope["coverage_partitions"] == {
        "input_preprocessing": 8,
        "declaration_lists": 10,
        "at_rules": 5,
        "declaration_recovery": 4,
        "fail_closed_boundaries": 6,
    }
    assert scope["required_platforms"] == ["ubuntu-24.04", "windows-2022"]
    assert scope["configured_parser_profile_claim"] is True
    assert scope["full_css_syntax_wpt_claim"] is False
    assert scope["cssom_property_grammar_claim"] is False
    assert scope["selector_grammar_claim"] is False

    assert manifest["schema"] == "zevryon.z3-css-parser-corpus.v1"
    assert manifest["authority"] == "z3-css-parser-conformance-v1"
    assert manifest["case_count"] == 33
    assert len(manifest["cases"]) == 33
    assert len({case["id"] for case in manifest["cases"]}) == 33

    assert "EXPECTED_CASES = 33" in runner
    assert "unsupported" in runner
    assert "css_parser_conformance_claim" in runner
    assert "parse_css_stylesheet_v1" in probe
    assert "STATUS\\tOK" in probe
    assert "_setmode(_fileno(stdin), _O_BINARY)" in probe
    assert (
        "Windows probe stdin is forced to binary mode so CRLF and carriage-return bytes reach preprocessing unchanged"
        in scope["authority_contract"]
    )
    assert "zevryon-css-parser-v1-probe" in cmake

    program = json.loads(PROGRAM.read_text(encoding="utf-8"))
    z3 = next(item for item in program["milestones"] if item["id"] == "Z3")
    assert "css_parser_conformance" in z3["required_gates"]


if __name__ == "__main__":
    test_z3_css_parser_authority_scope()
    print("Z3 CSS parser authority scope contract passed")
