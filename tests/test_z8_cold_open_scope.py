from __future__ import annotations

import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCOPE = ROOT / "certification" / "z8_cold_open_scope.json"


def test_z8_cold_open_scope_is_narrow_and_frozen() -> None:
    data = json.loads(SCOPE.read_text(encoding="utf-8"))
    assert data["schema"] == "zevryon.z8-cold-open-scope.v1"
    assert data["milestone"] == "Z8"
    assert data["status_change"] is False
    assert data["admitted_gates"] == [
        "stable_node_identity",
        "cold_node_byte_budget",
        "lazy_wrapper_projection",
        "bounded_million_node_open",
    ]
    assert data["remaining_gates"] == ["dom_traversal_conformance"]
    assert data["million_node_fixture"] == {
        "node_count": 1_000_000,
        "cold_sidecar_bytes_per_node_max": 97,
        "cold_open_ms_max": 2_000,
        "tail_lookup_ms_max": 2_000,
    }
    assert data["full_dom_conformance_claim"] is False


if __name__ == "__main__":
    test_z8_cold_open_scope_is_narrow_and_frozen()
    print("Z8 cold-open scope contract passed")
