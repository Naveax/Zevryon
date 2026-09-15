import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROGRAM_PATH = ROOT / "config" / "zenith_program.json"
CLOSURE_PATH = ROOT / "certification" / "z1_gate_closure.json"

EXPECTED_GATES = [
    "streaming_utf8_chunk_equivalence",
    "grapheme_segmentation_conformance",
    "bidi_run_conformance",
    "line_break_conformance",
    "core_seek_regression_gate",
]
SHA_RE = re.compile(r"^[0-9a-f]{40}$")


def test_z1_gate_closure_contract():
    program = json.loads(PROGRAM_PATH.read_text(encoding="utf-8"))
    closure = json.loads(CLOSURE_PATH.read_text(encoding="utf-8"))

    z1 = next(item for item in program["milestones"] if item["id"] == "Z1")
    assert z1["status"] == "implemented"
    assert z1["dependencies"] == ["Z0"]
    assert z1["required_gates"] == EXPECTED_GATES

    assert closure["schema"] == "zevryon.z1-gate-closure.v1"
    assert closure["milestone"] == "Z1"
    assert closure["status"] == "implemented"
    assert list(closure["gates"]) == EXPECTED_GATES

    evidence = set(z1["evidence"])
    assert "certification:z1-gate-closure-v1" in evidence

    seen_runs = set()
    seen_merges = set()
    for gate_name in EXPECTED_GATES:
        gate = closure["gates"][gate_name]
        assert gate["status"] == "PASS"
        assert isinstance(gate["workflow_run"], int) and gate["workflow_run"] > 0
        assert SHA_RE.fullmatch(gate["head_sha"])
        assert SHA_RE.fullmatch(gate["merge_commit"])
        assert gate["workflow_run"] not in seen_runs
        assert gate["merge_commit"] not in seen_merges
        seen_runs.add(gate["workflow_run"])
        seen_merges.add(gate["merge_commit"])
        assert f"github-actions-run:{gate['workflow_run']}" in evidence
        assert f"merge-commit:{gate['merge_commit']}" in evidence

    assert closure["all_required_gates_passed"] is True
    assert closure["scope"] == "unicode-text-substrate"
