#!/usr/bin/env python3
from __future__ import annotations

import copy
from pathlib import Path
import sys

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT))

from zevryon_platform.competitor_lab import CORE_METRIC_NAMES, Engine  # noqa: E402
from zevryon_platform.competitor_lab_v2 import (  # noqa: E402
    canonical_workload_sha256,
    evaluate_campaign_payload,
)

CORPUS_SHA = "ab" * 32
WORKLOAD = {
    "version": 1,
    "viewport": {"width": 1440, "height": 900},
    "operations": [
        {"kind": "open_preindexed"},
        {"kind": "open_streaming"},
        {"kind": "scroll", "distance_css_px": 120000},
        {"kind": "exact_search", "mode": "warm"},
        {"kind": "exact_search", "mode": "cold"},
        {"kind": "mutation_batch", "count": 1000},
        {"kind": "copy_all"},
    ],
}
WORKLOAD_SHA = canonical_workload_sha256(WORKLOAD)
MEASURED = (
    Engine.ZEVRYON,
    Engine.CHROME,
    Engine.FIREFOX,
    Engine.EDGE,
    Engine.WEBKIT,
)


def require(value: bool, message: str) -> None:
    if not value:
        raise AssertionError(message)


def system_state() -> dict[str, object]:
    return {
        "os_name": "Linux",
        "os_release": "fixture",
        "architecture": "x86_64",
        "cpu_model": "Zevryon competitor fixture CPU",
        "physical_ram_mib": 16384,
        "thermal_state": "nominal",
        "power_mode": "performance",
    }


def metrics(engine: Engine) -> dict[str, float]:
    values: dict[str, float] = {}
    for index, name in enumerate(CORE_METRIC_NAMES):
        if engine != Engine.ZEVRYON:
            values[name] = 100.0
        elif index < 4:
            values[name] = 90.0
        elif name == "copy_throughput_mib_s":
            values[name] = 96.0
        else:
            values[name] = 104.0
    return values


def campaign() -> dict[str, object]:
    statuses = {
        engine.value: {"status": "measured", "reason": ""}
        for engine in Engine
    }
    statuses[Engine.SERVO.value] = {
        "status": "unsupported",
        "reason": "fixture marks Servo unsupported",
    }
    statuses[Engine.LADYBIRD.value] = {
        "status": "unsupported",
        "reason": "fixture marks Ladybird unsupported",
    }
    runs: list[dict[str, object]] = []
    for engine in MEASURED:
        for index in range(5):
            runs.append(
                {
                    "engine": engine.value,
                    "engine_version": "fixture-1",
                    "corpus_sha256": CORPUS_SHA,
                    "corpus_logical_bytes": 4096,
                    "workload_sha256": WORKLOAD_SHA,
                    "captured_at_utc": f"2026-08-21T18:0{index}:00Z",
                    "run_index": index,
                    "system_state": system_state(),
                    "metrics": metrics(engine),
                    "failure_mode": None,
                }
            )
    return {"schema_version": 2, "statuses": statuses, "runs": runs}


def main() -> int:
    reordered = {
        "operations": [dict(reversed(list(item.items()))) for item in WORKLOAD["operations"]],
        "viewport": {"height": 900, "width": 1440},
        "version": 1,
    }
    require(
        canonical_workload_sha256(reordered) == WORKLOAD_SHA,
        "workload hash depends on object key ordering",
    )

    payload = campaign()
    result = evaluate_campaign_payload(payload)
    require(bool(result["leadership"]["leadership_claim_allowed"]), "v2 leadership")
    require(
        result["leadership"]["workload_sha256"] == WORKLOAD_SHA,
        "leadership did not expose workload identity",
    )
    require(
        all(
            aggregate["workload_sha256"] == WORKLOAD_SHA
            for aggregate in result["aggregates"].values()
        ),
        "aggregate lost workload identity",
    )

    mismatch = copy.deepcopy(payload)
    for run in mismatch["runs"]:
        if run["engine"] == Engine.CHROME.value:
            run["workload_sha256"] = "cd" * 32
    mismatch_result = evaluate_campaign_payload(mismatch)
    require(
        not bool(mismatch_result["leadership"]["leadership_claim_allowed"]),
        "cross-engine workload mismatch was admitted",
    )
    require(
        "workload_identity_mismatch" in mismatch_result["leadership"]["blockers"],
        "workload mismatch blocker missing",
    )

    mixed_within_engine = copy.deepcopy(payload)
    mixed_within_engine["runs"][0]["workload_sha256"] = "ef" * 32
    try:
        evaluate_campaign_payload(mixed_within_engine)
    except ValueError as error:
        require("workload" in str(error), "wrong mixed-workload error")
    else:
        raise AssertionError("mixed workload within one engine was accepted")

    missing = copy.deepcopy(payload)
    del missing["runs"][0]["workload_sha256"]
    try:
        evaluate_campaign_payload(missing)
    except ValueError as error:
        require("workload_sha256" in str(error), "missing workload error lost")
    else:
        raise AssertionError("missing workload_sha256 was accepted")

    print("Zevryon M7 workload identity smoke passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
