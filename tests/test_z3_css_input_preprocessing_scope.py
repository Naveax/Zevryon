from __future__ import annotations

import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCOPE = ROOT / "certification" / "z3_css_input_preprocessing_scope.json"
PROGRAM = ROOT / "config" / "zenith_program.json"
HEADER = ROOT / "src" / "css_parser_v1.hpp"
SOURCE = ROOT / "src" / "css_parser_v1.cpp"


def test_z3_css_input_preprocessing_scope() -> None:
    scope = json.loads(SCOPE.read_text(encoding="utf-8"))
    assert scope["schema"] == "zevryon.z3-css-input-preprocessing-scope.v1"
    assert scope["milestone"] == "Z3"
    assert scope["status_change"] is False
    assert scope["gate_progress"] == ["css_parser_conformance"]
    assert scope["admitted_gates"] == []
    assert scope["resource_class"] == "ComputedStyle"

    wpt = scope["wpt_reference"]
    assert wpt["repository"] == "web-platform-tests/wpt"
    assert wpt["upstream_commit"] == "15df54d4459b78242d32ae36f9c094a96972bedc"
    assert wpt["upstream_path"] == "css/css-syntax/input-preprocessing.html"
    assert wpt["upstream_blob_sha"] == "9ef9a730820d85a015b51cb230aa09f397a78400"
    assert wpt["null_cases_admitted"] == 5
    assert wpt["surrogate_cssom_cases_admitted"] == 0

    cases = scope["authority_cases"]
    assert cases == {
        "wpt_null_cases": 5,
        "newline_normalization_cases": 1,
        "invalid_utf8_cases": 1,
    }

    program = json.loads(PROGRAM.read_text(encoding="utf-8"))
    z3 = next(item for item in program["milestones"] if item["id"] == "Z3")
    assert z3["status"] == "planned"
    assert z3["evidence"] == []
    assert "css_parser_conformance" in z3["required_gates"]

    header = HEADER.read_text(encoding="utf-8")
    source = SOURCE.read_text(encoding="utf-8")
    for field in (
        "preprocessed_input_bytes",
        "null_replacements",
        "newline_normalizations",
        "invalid_utf8_replacements",
    ):
        assert field in header
    assert "preprocess_css_input" in source
    assert "std::pmr::string preprocessed(output->resource())" in source
    assert r'output->append("\xef\xbf\xbd", 3U)' in source


if __name__ == "__main__":
    test_z3_css_input_preprocessing_scope()
    print("Z3 CSS input preprocessing scope contract passed")
