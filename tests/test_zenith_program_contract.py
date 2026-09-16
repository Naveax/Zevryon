from __future__ import annotations

import copy
import json
from pathlib import Path

import pytest

from scripts.zenith_program_contract import ContractError, load_and_validate, validate_program


ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "config" / "zenith_program.json"


def load_manifest() -> dict:
    return json.loads(MANIFEST.read_text(encoding="utf-8"))


def test_repository_manifest_is_valid() -> None:
    result = load_and_validate(MANIFEST)
    assert result["ok"] is True
    assert result["milestone_count"] == 16
    assert result["active"] == []
    assert result["status_counts"]["implemented"] == 5
    assert result["core_gates"]["source_read_bytes_max"] == 65_536

    program = load_manifest()
    z1 = program["milestones"][1]
    assert z1["id"] == "Z1"
    assert z1["status"] == "implemented"
    assert z1["required_gates"] == [
        "streaming_utf8_chunk_equivalence",
        "grapheme_segmentation_conformance",
        "bidi_run_conformance",
        "line_break_conformance",
        "core_seek_regression_gate",
    ]
    assert "certification:z1-gate-closure-v1" in z1["evidence"]

    z2 = program["milestones"][2]
    assert z2["id"] == "Z2"
    assert z2["status"] == "implemented"
    assert z2["dependencies"] == ["Z1"]
    assert z2["required_gates"] == [
        "font_fallback_correctness",
        "glyph_cluster_roundtrip",
        "bounded_glyph_cache",
        "visible_shaping_p95",
        "pixel_reference_tests",
    ]
    assert z2["evidence"] == [
        "certification:z2-gate-closure-v1",
        "github-actions-run:35079429233",
        "merge-commit:036e939109800dcf19088ccac9145222e2a77e0b",
    ]

    z7 = program["milestones"][7]
    assert z7["id"] == "Z7"
    assert z7["status"] == "implemented"
    assert z7["required_gates"] == [
        "html_tokenizer_conformance",
        "tree_builder_conformance",
        "chunk_size_equivalence",
        "parser_fuzzing",
        "bounded_large_document_parse",
    ]
    assert len(z7["evidence"]) == 8

    z8 = program["milestones"][8]
    assert z8["id"] == "Z8"
    assert z8["status"] == "implemented"
    assert z8["dependencies"] == ["Z7"]
    assert z8["required_gates"] == [
        "stable_node_identity",
        "cold_node_byte_budget",
        "lazy_wrapper_projection",
        "dom_traversal_conformance",
        "bounded_million_node_open",
    ]
    assert z8["evidence"][0] == "certification:z8-gate-closure-v1"
    assert len(z8["evidence"]) == 5


def test_implemented_milestone_requires_evidence() -> None:
    program = load_manifest()
    program["milestones"][0]["evidence"] = []
    with pytest.raises(ContractError, match="without evidence"):
        validate_program(program)


def test_active_milestone_cannot_skip_dependency() -> None:
    program = load_manifest()
    program["milestones"][0]["status"] = "planned"
    program["milestones"][0]["evidence"] = []
    with pytest.raises(ContractError, match="unimplemented dependency"):
        validate_program(program)


def test_dependency_cycle_is_rejected() -> None:
    program = load_manifest()
    program["milestones"][0]["status"] = "planned"
    program["milestones"][0]["evidence"] = []
    program["milestones"][1]["status"] = "planned"
    program["milestones"][1]["evidence"] = []
    program["milestones"][2]["status"] = "planned"
    program["milestones"][2]["evidence"] = []
    program["milestones"][7]["status"] = "planned"
    program["milestones"][7]["evidence"] = []
    program["milestones"][8]["status"] = "planned"
    program["milestones"][8]["evidence"] = []
    program["milestones"][0]["dependencies"] = ["Z15"]
    with pytest.raises(ContractError, match="cycle"):
        validate_program(program)


def test_core_performance_gates_cannot_be_weakened() -> None:
    program = copy.deepcopy(load_manifest())
    program["policy"]["core_random_p95_ms_max"] = 0.51
    with pytest.raises(ContractError, match="random P95 gate weakened"):
        validate_program(program)

    program = copy.deepcopy(load_manifest())
    program["policy"]["core_source_read_bytes_max"] = 65_537
    with pytest.raises(ContractError, match="source-read gate weakened"):
        validate_program(program)
