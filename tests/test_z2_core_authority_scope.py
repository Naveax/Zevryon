from __future__ import annotations

import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCOPE = ROOT / "certification" / "z2_core_authority_scope.json"


def test_z2_core_authority_scope_is_exact() -> None:
    data = json.loads(SCOPE.read_text(encoding="utf-8"))
    assert data["schema"] == "zevryon.z2-core-authority-scope.v1"
    assert data["milestone"] == "Z2"
    assert data["status_change"] is False
    assert data["admitted_gates"] == [
        "font_fallback_correctness",
        "glyph_cluster_roundtrip",
        "bounded_glyph_cache",
        "visible_shaping_p95",
        "pixel_reference_tests",
    ]
    assert data["visible_shaping"] == {
        "production_path": "cache-backed prepared HarfBuzz catalog shaping",
        "p95_ms_max": 1.5,
        "p99_ms_max": 2.5,
        "requires_semantic_equivalence_with_plain_and_direct_prepared_paths": True,
    }
    assert data["pixel_reference_surface"] == "shader draw packet equivalence oracle"
    assert data["full_browser_text_stack_claim"] is False


if __name__ == "__main__":
    test_z2_core_authority_scope_is_exact()
    print("Z2 core authority scope contract passed")
