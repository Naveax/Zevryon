#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any

EXPECTED_IDS = [f"Z{index}" for index in range(16)]
ALLOWED_STATUSES = {"planned", "active", "implemented"}
REQUIRED_POLICY_KEYS = {
    "forbid_skipped_dependencies",
    "require_evidence_for_implemented",
    "require_hard_budget_for_resident_cache",
    "core_random_p95_ms_max",
    "core_adjacent_p95_ms_max",
    "core_random_p99_ms_max",
    "core_source_read_bytes_max",
    "checkpoint_overhead_ratio_max",
    "silent_corruption_max",
    "payload_data_loss_bytes_max",
}


class ContractError(ValueError):
    pass


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise ContractError(message)


def validate_program(program: dict[str, Any]) -> dict[str, Any]:
    _require(
        program.get("schema") == "zevryon.zenith-program.v1",
        "unexpected ZENITH program schema",
    )
    policy = program.get("policy")
    _require(isinstance(policy, dict), "policy must be an object")
    _require(
        REQUIRED_POLICY_KEYS.issubset(policy),
        "policy is missing required hard-gate keys",
    )
    _require(policy["forbid_skipped_dependencies"] is True, "dependencies must be enforced")
    _require(
        policy["require_evidence_for_implemented"] is True,
        "implemented milestones must require evidence",
    )
    _require(
        policy["require_hard_budget_for_resident_cache"] is True,
        "resident caches must require hard budgets",
    )
    _require(float(policy["core_random_p95_ms_max"]) <= 0.5, "core random P95 gate weakened")
    _require(float(policy["core_adjacent_p95_ms_max"]) <= 0.3, "adjacent P95 gate weakened")
    _require(float(policy["core_random_p99_ms_max"]) <= 0.75, "core random P99 gate weakened")
    _require(
        int(policy["core_source_read_bytes_max"]) <= 65_536,
        "core source-read gate weakened",
    )
    _require(
        float(policy["checkpoint_overhead_ratio_max"]) <= 0.002,
        "checkpoint overhead gate weakened",
    )
    _require(int(policy["silent_corruption_max"]) == 0, "silent corruption must remain zero")
    _require(
        int(policy["payload_data_loss_bytes_max"]) == 0,
        "payload data loss must remain zero",
    )

    milestones = program.get("milestones")
    _require(isinstance(milestones, list), "milestones must be an array")
    _require(len(milestones) == 16, "program must contain exactly Z0 through Z15")

    ids = [milestone.get("id") for milestone in milestones]
    _require(ids == EXPECTED_IDS, "milestone IDs must be ordered Z0 through Z15")
    _require(len(set(ids)) == len(ids), "milestone IDs must be unique")

    by_id = {milestone["id"]: milestone for milestone in milestones}
    active_ids: list[str] = []
    for milestone in milestones:
        milestone_id = milestone["id"]
        name = milestone.get("name")
        status = milestone.get("status")
        dependencies = milestone.get("dependencies")
        gates = milestone.get("required_gates")
        evidence = milestone.get("evidence")

        _require(isinstance(name, str) and name.strip(), f"{milestone_id} requires a name")
        _require(status in ALLOWED_STATUSES, f"{milestone_id} has invalid status")
        _require(isinstance(dependencies, list), f"{milestone_id} dependencies must be an array")
        _require(isinstance(gates, list) and gates, f"{milestone_id} requires gates")
        _require(isinstance(evidence, list), f"{milestone_id} evidence must be an array")
        _require(len(dependencies) == len(set(dependencies)), f"{milestone_id} has duplicate dependencies")
        _require(len(gates) == len(set(gates)), f"{milestone_id} has duplicate gates")
        _require(milestone_id not in dependencies, f"{milestone_id} depends on itself")
        _require(all(dependency in by_id for dependency in dependencies), f"{milestone_id} has unknown dependency")
        _require(all(isinstance(gate, str) and gate.strip() for gate in gates), f"{milestone_id} has invalid gate")
        _require(all(isinstance(item, str) and item.strip() for item in evidence), f"{milestone_id} has invalid evidence")

        if status == "active":
            active_ids.append(milestone_id)
            _require(
                all(by_id[dependency]["status"] == "implemented" for dependency in dependencies),
                f"{milestone_id} cannot be active before its dependencies are implemented",
            )
        elif status == "implemented":
            _require(evidence, f"{milestone_id} is implemented without evidence")
            _require(
                all(by_id[dependency]["status"] == "implemented" for dependency in dependencies),
                f"{milestone_id} skips an unimplemented dependency",
            )
        else:
            _require(not evidence, f"planned milestone {milestone_id} must not claim evidence")

    _require(len(active_ids) <= 1, "only one milestone may be active in the single-owner program")

    visiting: set[str] = set()
    visited: set[str] = set()

    def visit(milestone_id: str) -> None:
        if milestone_id in visited:
            return
        _require(milestone_id not in visiting, "milestone dependency graph contains a cycle")
        visiting.add(milestone_id)
        for dependency in by_id[milestone_id]["dependencies"]:
            visit(dependency)
        visiting.remove(milestone_id)
        visited.add(milestone_id)

    for milestone_id in EXPECTED_IDS:
        visit(milestone_id)

    status_counts = {
        status: sum(1 for milestone in milestones if milestone["status"] == status)
        for status in sorted(ALLOWED_STATUSES)
    }
    return {
        "schema": "zevryon.zenith-program-validation.v1",
        "ok": True,
        "milestone_count": len(milestones),
        "active": active_ids,
        "status_counts": status_counts,
        "core_gates": {
            "random_p95_ms_max": float(policy["core_random_p95_ms_max"]),
            "adjacent_p95_ms_max": float(policy["core_adjacent_p95_ms_max"]),
            "random_p99_ms_max": float(policy["core_random_p99_ms_max"]),
            "source_read_bytes_max": int(policy["core_source_read_bytes_max"]),
            "checkpoint_overhead_ratio_max": float(policy["checkpoint_overhead_ratio_max"]),
        },
    }


def load_and_validate(path: Path) -> dict[str, Any]:
    return validate_program(json.loads(path.read_text(encoding="utf-8")))


def main() -> int:
    parser = argparse.ArgumentParser(description="Validate the ZENITH Z0-Z15 dependency contract")
    parser.add_argument(
        "--manifest",
        type=Path,
        default=Path("config/zenith_program.json"),
    )
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    result = load_and_validate(args.manifest)
    text = json.dumps(result, indent=2, sort_keys=True) + "\n"
    if args.output is not None:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text, encoding="utf-8")
    print(text, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
