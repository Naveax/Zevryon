from __future__ import annotations

import json
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PROGRAM = ROOT / "config" / "zenith_program.json"
CLOSURE = ROOT / "certification" / "z3_gate_closure.json"

SCOPES = {
    "css_parser_conformance": ROOT / "certification" / "z3_css_parser_authority_scope.json",
    "cascade_conformance": ROOT / "certification" / "z3_cascade_authority_scope.json",
    "selector_dependency_invalidation": ROOT / "certification" / "z3_selector_invalidation_authority_scope.json",
    "bounded_style_dag": ROOT / "certification" / "z3_style_dag_authority_scope.json",
    "offscreen_materialization_bound": ROOT / "certification" / "z3_offscreen_materialization_authority_scope.json",
}
EXPECTED_GATES = list(SCOPES)
SHA_RE = re.compile(r"^[0-9a-f]{40}$")


def test_z3_gate_closure_contract() -> None:
    program = json.loads(PROGRAM.read_text(encoding="utf-8"))
    closure = json.loads(CLOSURE.read_text(encoding="utf-8"))
    scopes = {
        gate: json.loads(path.read_text(encoding="utf-8"))
        for gate, path in SCOPES.items()
    }

    z3 = next(item for item in program["milestones"] if item["id"] == "Z3")
    assert z3["status"] == "implemented"
    assert z3["dependencies"] == ["Z0", "Z8"]
    assert z3["required_gates"] == EXPECTED_GATES

    assert closure["schema"] == "zevryon.z3-gate-closure.v1"
    assert closure["milestone"] == "Z3"
    assert closure["status"] == "implemented"
    assert closure["scope"] == "css-parser-cascade-style-sharing"
    assert closure["all_required_gates_passed"] is True
    assert list(closure["gates"]) == EXPECTED_GATES

    evidence = set(z3["evidence"])
    assert "certification:z3-gate-closure-v1" in evidence

    for gate_name in EXPECTED_GATES:
        scope = scopes[gate_name]
        gate = closure["gates"][gate_name]

        assert scope["status_change"] is False
        assert scope["admitted_gates"] == [gate_name]
        assert gate["status"] == "PASS"
        assert gate["authority_scope"] == scope["schema"]
        assert gate["authority"] == scope.get("authority", scope["schema"])
        assert isinstance(gate["workflow_run"], int) and gate["workflow_run"] > 0
        assert SHA_RE.fullmatch(gate["head_sha"])
        assert SHA_RE.fullmatch(gate["merge_commit"])
        assert isinstance(gate["source_pr"], int) and gate["source_pr"] > 0
        assert f"github-actions-run:{gate['workflow_run']}" in evidence
        assert f"merge-commit:{gate['merge_commit']}" in evidence


if __name__ == "__main__":
    test_z3_gate_closure_contract()
    print("Z3 gate closure contract passed")
