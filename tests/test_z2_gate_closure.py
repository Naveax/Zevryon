import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROGRAM_PATH = ROOT / "config" / "zenith_program.json"
CLOSURE_PATH = ROOT / "certification" / "z2_gate_closure.json"
SCOPE_PATH = ROOT / "certification" / "z2_core_authority_scope.json"

EXPECTED_GATES = [
    "font_fallback_correctness",
    "glyph_cluster_roundtrip",
    "bounded_glyph_cache",
    "visible_shaping_p95",
    "pixel_reference_tests",
]
AUTHORITY_RUN = 35079429233
SUPPORTING_CI_RUN = 35079429249
AUTHORITY_HEAD = "8ebb326360ed477efc6ba52c14d89ff402ce7406"
AUTHORITY_MERGE = "036e939109800dcf19088ccac9145222e2a77e0b"
SHA_RE = re.compile(r"^[0-9a-f]{40}$")


def test_z2_gate_closure_contract() -> None:
    program = json.loads(PROGRAM_PATH.read_text(encoding="utf-8"))
    closure = json.loads(CLOSURE_PATH.read_text(encoding="utf-8"))
    scope = json.loads(SCOPE_PATH.read_text(encoding="utf-8"))

    z2 = next(item for item in program["milestones"] if item["id"] == "Z2")
    assert z2["status"] == "implemented"
    assert z2["dependencies"] == ["Z1"]
    assert z2["required_gates"] == EXPECTED_GATES

    assert scope["schema"] == "zevryon.z2-core-authority-scope.v1"
    assert scope["milestone"] == "Z2"
    assert scope["status_change"] is False
    assert scope["admitted_gates"] == EXPECTED_GATES
    assert scope["full_browser_text_stack_claim"] is False

    assert closure["schema"] == "zevryon.z2-gate-closure.v1"
    assert closure["milestone"] == "Z2"
    assert closure["status"] == "implemented"
    assert closure["scope"] == "font-discovery-and-shaping"
    assert closure["source_scope"] == "certification/z2_core_authority_scope.json"
    assert closure["supporting_general_ci_run"] == SUPPORTING_CI_RUN
    assert closure["all_required_gates_passed"] is True
    assert list(closure["gates"]) == EXPECTED_GATES

    evidence = set(z2["evidence"])
    assert "certification:z2-gate-closure-v1" in evidence
    assert f"github-actions-run:{AUTHORITY_RUN}" in evidence
    assert f"merge-commit:{AUTHORITY_MERGE}" in evidence

    for gate_name in EXPECTED_GATES:
        gate = closure["gates"][gate_name]
        assert gate["status"] == "PASS"
        assert gate["authority"] == "z2-core-authority-scope-v1"
        assert gate["workflow_run"] == AUTHORITY_RUN
        assert gate["head_sha"] == AUTHORITY_HEAD
        assert gate["merge_commit"] == AUTHORITY_MERGE
        assert SHA_RE.fullmatch(gate["head_sha"])
        assert SHA_RE.fullmatch(gate["merge_commit"])


if __name__ == "__main__":
    test_z2_gate_closure_contract()
    print("Z2 gate closure contract passed")
