from __future__ import annotations

import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PROGRAM = ROOT / "config" / "zenith_program.json"
CLOSURE = ROOT / "certification" / "z3_gate_closure.json"

SCOPE_PATHS = [
    ROOT / "certification" / "z3_css_parser_authority_scope.json",
    ROOT / "certification" / "z3_cascade_authority_scope.json",
    ROOT / "certification" / "z3_selector_invalidation_authority_scope.json",
    ROOT / "certification" / "z3_style_dag_authority_scope.json",
    ROOT / "certification" / "z3_offscreen_materialization_authority_scope.json",
]


def test_z3_authorities_cover_exact_program_gates_after_promotion() -> None:
    program = json.loads(PROGRAM.read_text(encoding="utf-8"))
    closure = json.loads(CLOSURE.read_text(encoding="utf-8"))
    scopes = [
        json.loads(path.read_text(encoding="utf-8"))
        for path in SCOPE_PATHS
    ]

    z3 = next(item for item in program["milestones"] if item["id"] == "Z3")
    assert z3["status"] == "implemented"
    assert z3["dependencies"] == ["Z0", "Z8"]

    admitted: set[str] = set()
    for scope in scopes:
        assert scope["milestone"] == "Z3"
        assert scope["status_change"] is False
        admitted.update(scope["admitted_gates"])

    assert admitted == set(z3["required_gates"])
    assert len(admitted) == len(z3["required_gates"])
    assert closure["all_required_gates_passed"] is True
    assert list(closure["gates"]) == z3["required_gates"]
    assert "certification:z3-gate-closure-v1" in z3["evidence"]


if __name__ == "__main__":
    test_z3_authorities_cover_exact_program_gates_after_promotion()
    print("Z3 authority aggregation and promotion contract passed")
