from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
from typing import Iterable, Mapping, Sequence

from zevryon_platform.competitor_lab import (
    Engine,
    EngineAggregate,
    EngineCampaignStatus,
    EngineStatus,
    CompetitorRun,
    aggregate_engine_runs,
    evaluate_campaign_payload as evaluate_campaign_payload_v1,
    evaluate_leadership,
    run_from_mapping,
    status_from_mapping,
)

SCHEMA_VERSION = 2


def _valid_sha256(value: str) -> bool:
    return len(value) == 64 and all(ch in "0123456789abcdef" for ch in value)


def canonical_workload_sha256(payload: Mapping[str, object]) -> str:
    canonical = json.dumps(
        payload,
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=False,
    ).encode("utf-8")
    return hashlib.sha256(canonical).hexdigest()


@dataclass(frozen=True)
class WorkloadBoundRun:
    run: CompetitorRun
    workload_sha256: str

    def validate(self) -> None:
        self.run.validate()
        if not _valid_sha256(self.workload_sha256):
            raise ValueError("workload_sha256 must be lowercase hexadecimal SHA-256")

    def to_dict(self) -> dict[str, object]:
        self.validate()
        result = self.run.to_dict()
        result["workload_sha256"] = self.workload_sha256
        return result


@dataclass(frozen=True)
class WorkloadBoundAggregate:
    aggregate: EngineAggregate
    workload_sha256: str

    def validate(self) -> None:
        if not _valid_sha256(self.workload_sha256):
            raise ValueError("workload_sha256 must be lowercase hexadecimal SHA-256")

    def to_dict(self) -> dict[str, object]:
        self.validate()
        result = self.aggregate.to_dict()
        result["workload_sha256"] = self.workload_sha256
        return result


def bound_run_from_mapping(value: Mapping[str, object]) -> WorkloadBoundRun:
    if "workload_sha256" not in value:
        raise ValueError("competitor run is missing workload_sha256")
    workload_sha = value["workload_sha256"]
    if not isinstance(workload_sha, str):
        raise ValueError("workload_sha256 must be a string")
    v1_value = dict(value)
    del v1_value["workload_sha256"]
    bound = WorkloadBoundRun(run_from_mapping(v1_value), workload_sha)
    bound.validate()
    return bound


def aggregate_bound_engine_runs(
    runs: Sequence[WorkloadBoundRun],
) -> WorkloadBoundAggregate:
    if not runs:
        raise ValueError("engine run set must not be empty")
    for run in runs:
        run.validate()
    workload_sha = runs[0].workload_sha256
    if any(run.workload_sha256 != workload_sha for run in runs):
        raise ValueError("engine aggregate mixes workload identities")
    aggregate = aggregate_engine_runs([run.run for run in runs])
    return WorkloadBoundAggregate(aggregate, workload_sha)


def campaign_sha256(runs: Iterable[WorkloadBoundRun]) -> str:
    ordered = sorted(
        runs,
        key=lambda item: (
            item.run.engine.value,
            item.run.run_index,
            item.run.captured_at_utc,
        ),
    )
    payload = [item.to_dict() for item in ordered]
    canonical = json.dumps(
        payload,
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=False,
    ).encode("utf-8")
    return hashlib.sha256(canonical).hexdigest()


def evaluate_bound_leadership(
    aggregates: Mapping[Engine, WorkloadBoundAggregate],
    statuses: Mapping[Engine, EngineCampaignStatus],
) -> dict[str, object]:
    base = evaluate_leadership(
        {engine: item.aggregate for engine, item in aggregates.items()},
        statuses,
    )
    measured = [
        engine
        for engine in Engine
        if statuses[engine].status == EngineStatus.MEASURED
    ]
    workloads = {aggregates[engine].workload_sha256 for engine in measured}
    same_workload = len(workloads) == 1
    checks = dict(base["checks"])
    checks["same_workload_identity"] = same_workload
    base["checks"] = checks
    if not same_workload:
        blockers = set(base["blockers"])
        blockers.add("workload_identity_mismatch")
        base["blockers"] = sorted(blockers)
    base["leadership_claim_allowed"] = (
        bool(base["leadership_claim_allowed"]) and same_workload
    )
    base["workload_sha256"] = next(iter(workloads)) if same_workload else None
    return base


def evaluate_campaign_payload(payload: Mapping[str, object]) -> dict[str, object]:
    if set(payload) != {"schema_version", "statuses", "runs"}:
        raise ValueError("competitor campaign fields mismatch")
    if payload["schema_version"] != SCHEMA_VERSION:
        raise ValueError("unsupported workload-bound competitor campaign schema")

    statuses_raw = payload["statuses"]
    if not isinstance(statuses_raw, Mapping):
        raise ValueError("statuses must be an object")
    if set(statuses_raw) != {engine.value for engine in Engine}:
        raise ValueError("statuses must cover every canonical engine")
    statuses: dict[Engine, EngineCampaignStatus] = {}
    for engine in Engine:
        raw = statuses_raw[engine.value]
        if not isinstance(raw, Mapping):
            raise ValueError(f"status {engine.value} must be an object")
        statuses[engine] = status_from_mapping(raw)

    runs_raw = payload["runs"]
    if not isinstance(runs_raw, list):
        raise ValueError("runs must be an array")
    bound_runs: list[WorkloadBoundRun] = []
    for raw in runs_raw:
        if not isinstance(raw, Mapping):
            raise ValueError("run must be an object")
        bound_runs.append(bound_run_from_mapping(raw))

    grouped: dict[Engine, list[WorkloadBoundRun]] = {engine: [] for engine in Engine}
    for item in bound_runs:
        grouped[item.run.engine].append(item)

    aggregates: dict[Engine, WorkloadBoundAggregate] = {}
    for engine in Engine:
        status = statuses[engine].status
        engine_runs = grouped[engine]
        if status == EngineStatus.MEASURED:
            aggregates[engine] = aggregate_bound_engine_runs(engine_runs)
        elif engine_runs and status == EngineStatus.UNSUPPORTED:
            raise ValueError("unsupported engine must not contain benchmark runs")

    leadership = evaluate_bound_leadership(aggregates, statuses)
    failure_modes = {
        engine.value: [
            item.run.failure_mode
            for item in grouped[engine]
            if item.run.failure_mode is not None
        ]
        for engine in Engine
    }
    return {
        "schema_version": SCHEMA_VERSION,
        "campaign_sha256": campaign_sha256(bound_runs),
        "raw_run_count": len(bound_runs),
        "statuses": {
            engine.value: statuses[engine].to_dict()
            for engine in Engine
        },
        "aggregates": {
            engine.value: aggregates[engine].to_dict()
            for engine in Engine
            if engine in aggregates
        },
        "failure_modes": failure_modes,
        "leadership": leadership,
    }


def evaluate_campaign_payload_compatible(payload: Mapping[str, object]) -> dict[str, object]:
    schema_version = payload.get("schema_version")
    if schema_version == 1:
        return evaluate_campaign_payload_v1(payload)
    if schema_version == SCHEMA_VERSION:
        return evaluate_campaign_payload(payload)
    raise ValueError("unsupported competitor campaign schema")
