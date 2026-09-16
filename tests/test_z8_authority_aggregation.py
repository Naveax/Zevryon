from __future__ import annotations

import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PROGRAM = ROOT / "config" / "zenith_program.json"
COLD = ROOT / "certification" / "z8_cold_open_scope.json"
TRAVERSAL = ROOT / "certification" / "z8_dom_traversal_scope.json"


def test_z8_authorities_cover_exact_program_gates_without_promotion() -> None:
    program = json.loads(PROGRAM.read_text(encoding="utf-8"))
    cold = json.loads(COLD.read_text(encoding="utf-8"))
    traversal = json.loads(TRAVERSAL.read_text(encoding="utf-8"))

    z8 = next(item for item in program["milestones"] if item["id"] == "Z8")
    assert z8["status"] == "planned"
    assert z8["dependencies"] == ["Z7"]
    assert z8["required_gates"] == [
        "stable_node_identity",
        "cold_node_byte_budget",
        "lazy_wrapper_projection",
        "dom_traversal_conformance",
        "bounded_million_node_open",
    ]

    assert cold["status_change"] is False
    assert traversal["status_change"] is False
    admitted = set(cold["admitted_gates"])
    admitted.add(traversal["gate"])
    assert admitted == set(z8["required_gates"])

    assert cold["remaining_gates"] == [traversal["gate"]]
    assert cold["full_dom_conformance_claim"] is False
    assert traversal["webidl_dom_claim"] is False
    assert traversal["javascript_wrapper_claim"] is False


if __name__ == "__main__":
    test_z8_authorities_cover_exact_program_gates_without_promotion()
    print("Z8 authority aggregation contract passed")
