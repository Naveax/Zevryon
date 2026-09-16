import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROGRAM_PATH = ROOT / "config" / "zenith_program.json"
CLOSURE_PATH = ROOT / "certification" / "z8_gate_closure.json"
COLD_SCOPE_PATH = ROOT / "certification" / "z8_cold_open_scope.json"
TRAVERSAL_SCOPE_PATH = ROOT / "certification" / "z8_dom_traversal_scope.json"

EXPECTED_GATES = [
    "stable_node_identity",
    "cold_node_byte_budget",
    "lazy_wrapper_projection",
    "dom_traversal_conformance",
    "bounded_million_node_open",
]
COLD_GATES = {
    "stable_node_identity",
    "cold_node_byte_budget",
    "lazy_wrapper_projection",
    "bounded_million_node_open",
}
SHA_RE = re.compile(r"^[0-9a-f]{40}$")


def test_z8_gate_closure_contract() -> None:
    program = json.loads(PROGRAM_PATH.read_text(encoding="utf-8"))
    closure = json.loads(CLOSURE_PATH.read_text(encoding="utf-8"))
    cold = json.loads(COLD_SCOPE_PATH.read_text(encoding="utf-8"))
    traversal = json.loads(TRAVERSAL_SCOPE_PATH.read_text(encoding="utf-8"))

    z8 = next(item for item in program["milestones"] if item["id"] == "Z8")
    assert z8["status"] == "implemented"
    assert z8["dependencies"] == ["Z7"]
    assert z8["required_gates"] == EXPECTED_GATES

    assert closure["schema"] == "zevryon.z8-gate-closure.v1"
    assert closure["milestone"] == "Z8"
    assert closure["status"] == "implemented"
    assert closure["scope"] == "logical-dom-hot-projection"
    assert closure["all_required_gates_passed"] is True
    assert list(closure["gates"]) == EXPECTED_GATES

    assert cold["admitted_gates"] == [
        "stable_node_identity",
        "cold_node_byte_budget",
        "lazy_wrapper_projection",
        "bounded_million_node_open",
    ]
    assert cold["remaining_gates"] == ["dom_traversal_conformance"]
    assert cold["status_change"] is False
    assert traversal["gate"] == "dom_traversal_conformance"
    assert traversal["status_change"] is False
    assert set(cold["admitted_gates"]) | {traversal["gate"]} == set(EXPECTED_GATES)

    evidence = set(z8["evidence"])
    assert "certification:z8-gate-closure-v1" in evidence

    for gate_name in EXPECTED_GATES:
        gate = closure["gates"][gate_name]
        assert gate["status"] == "PASS"
        assert isinstance(gate["workflow_run"], int) and gate["workflow_run"] > 0
        assert SHA_RE.fullmatch(gate["head_sha"])
        assert SHA_RE.fullmatch(gate["merge_commit"])
        assert f"github-actions-run:{gate['workflow_run']}" in evidence
        assert f"merge-commit:{gate['merge_commit']}" in evidence
        if gate_name in COLD_GATES:
            assert gate["authority"] == "z8-cold-open-scope-v1"
        else:
            assert gate["authority"] == "z8-dom-traversal-scope-v1"
