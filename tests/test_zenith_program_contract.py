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
    assert result["active"] == ["Z1"]
    assert result["status_counts"]["implemented"] == 1
    assert result["core_gates"]["source_read_bytes_max"] == 65_536


def test_implemented_milestone_requires_evidence() -> None:
    program = load_manifest()
    program["milestones"][0]["evidence"] = []
    with pytest.raises(ContractError, match="without evidence"):
        validate_program(program)


def test_active_milestone_cannot_skip_dependency() -> None:
    program = load_manifest()
    program["milestones"][0]["status"] = "planned"
    program["milestones"][0]["evidence"] = []
    with pytest.raises(ContractError, match="dependencies are implemented"):
        validate_program(program)


def test_dependency_cycle_is_rejected() -> None:
    program = load_manifest()
    program["milestones"][0]["status"] = "planned"
    program["milestones"][0]["evidence"] = []
    program["milestones"][1]["status"] = "planned"
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
