#!/usr/bin/env python3
from __future__ import annotations

import copy
from pathlib import Path
import sys

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT))

from zevryon_platform.competitor_lab import (  # noqa: E402
    CORE_METRIC_NAMES,
    Engine,
    campaign_sha256,
    evaluate_campaign_payload,
    nearest_rank,
    run_from_mapping,
)

CORPUS_SHA = "ab" * 32
MEASURED = (
    Engine.ZEVRYON,
    Engine.CHROME,
    Engine.FIREFOX,
    Engine.EDGE,
    Engine.WEBKIT,
)


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


def metrics(engine: Engine, *, bad_gap: bool = False) -> dict[str, float]:
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
    if engine == Engine.ZEVRYON and bad_gap:
        values["maximum_normal_stall_ms"] = 106.0
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
                    "captured_at_utc": f"2026-08-21T18:0{index}:00Z",
                    "run_index": index,
                    "system_state": system_state(),
                    "metrics": metrics(engine),
                    "failure_mode": None,
                }
            )
    return {"schema_version": 1, "statuses": statuses, "runs": runs}


def require(value: bool, message: str) -> None:
    if not value:
        raise AssertionError(message)


def main() -> int:
    require(nearest_rank([1.0, 2.0, 3.0, 4.0, 5.0], 95.0) == 5.0, "nearest-rank p95")

    payload = campaign()
    result = evaluate_campaign_payload(payload)
    leadership = result["leadership"]
    require(bool(leadership["leadership_claim_allowed"]), "positive leadership fixture")
    require(int(leadership["first_metric_count"]) == 4, "first-metric count")
    require(int(result["raw_run_count"]) == 25, "raw run count")

    parsed_runs = [run_from_mapping(raw) for raw in payload["runs"]]
    require(
        campaign_sha256(parsed_runs) == campaign_sha256(list(reversed(parsed_runs))),
        "campaign hash depends on input ordering",
    )

    bad_gap = copy.deepcopy(payload)
    for run in bad_gap["runs"]:
        if run["engine"] == Engine.ZEVRYON.value:
            run["metrics"]["maximum_normal_stall_ms"] = 106.0
    require(
        not bool(
            evaluate_campaign_payload(bad_gap)["leadership"]["leadership_claim_allowed"]
        ),
        "greater-than-five-percent gap was admitted",
    )

    missing = copy.deepcopy(payload)
    missing["statuses"][Engine.SERVO.value] = {
        "status": "missing",
        "reason": "fixture intentionally missing",
    }
    require(
        not bool(
            evaluate_campaign_payload(missing)["leadership"]["leadership_claim_allowed"]
        ),
        "missing canonical competitor was admitted",
    )

    failed = copy.deepcopy(payload)
    failed["runs"].append(
        {
            "engine": Engine.ZEVRYON.value,
            "engine_version": "fixture-1",
            "corpus_sha256": CORPUS_SHA,
            "corpus_logical_bytes": 4096,
            "captured_at_utc": "2026-08-21T18:10:00Z",
            "run_index": 5,
            "system_state": system_state(),
            "metrics": {},
            "failure_mode": "fixture crash before metrics",
        }
    )
    failed_result = evaluate_campaign_payload(failed)
    require(
        not bool(failed_result["leadership"]["leadership_claim_allowed"]),
        "failed raw run was admitted for leadership",
    )
    require(
        failed_result["failure_modes"][Engine.ZEVRYON.value]
        == ["fixture crash before metrics"],
        "failure mode was not preserved",
    )

    print("Zevryon M7 competitor-lab contract smoke passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
