from __future__ import annotations

import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCOPE = ROOT / "certification" / "z8_dom_traversal_scope.json"


def test_z8_dom_traversal_scope_is_exact() -> None:
    data = json.loads(SCOPE.read_text(encoding="utf-8"))
    assert data["schema"] == "zevryon.z8-dom-traversal-scope.v1"
    assert data["milestone"] == "Z8"
    assert data["gate"] == "dom_traversal_conformance"
    assert data["status_change"] is False
    assert data["production_surface"] == (
        "LogicalDomTraversal over LogicalNodeArenaV2StoreBoundReader"
    )
    assert data["certified_relations"] == [
        "parent",
        "first_child",
        "next_sibling",
        "previous_sibling",
        "next_preorder",
        "previous_preorder",
        "strict_descendant",
        "document_order",
    ]
    assert data["bounded_hop_policy"] == {
        "default_maximum_hops": 1_048_576,
        "absolute_maximum_hops": 16_777_216,
    }
    assert data["platforms"] == ["ubuntu-24.04", "windows-2022"]
    assert data["webidl_dom_claim"] is False
    assert data["javascript_wrapper_claim"] is False


if __name__ == "__main__":
    test_z8_dom_traversal_scope_is_exact()
    print("Z8 DOM traversal scope contract passed")
