from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime
from enum import Enum
import hashlib
import json
import math
import statistics
from typing import Iterable, Mapping, Sequence

SCHEMA_VERSION = 1
MIN_SUCCESSFUL_RUNS_PER_ENGINE = 5
MIN_MEASURED_COMPETITORS_FOR_LEADERSHIP = 4
LEADERSHIP_MAX_GAP_FRACTION = 0.05
LEADERSHIP_MIN_FIRST_METRICS = 4


class Engine(str, Enum):
    ZEVRYON = "zevryon"
    CHROME = "chrome"
    FIREFOX = "firefox"
    EDGE = "edge"
    WEBKIT = "webkit"
    SERVO = "servo"
    LADYBIRD = "ladybird"


class EngineStatus(str, Enum):
    MEASURED = "measured"
    UNSUPPORTED = "unsupported"
    FAILED = "failed"
    MISSING = "missing"


class MetricDirection(str, Enum):
    LOWER = "lower"
    HIGHER = "higher"


@dataclass(frozen=True)
class MetricSpec:
    name: str
    direction: MetricDirection


CORE_METRICS = (
    MetricSpec("process_group_pss_mb", MetricDirection.LOWER),
    MetricSpec("first_viewport_preindexed_ms", MetricDirection.LOWER),
    MetricSpec("first_viewport_streaming_ms", MetricDirection.LOWER),
    MetricSpec("scroll_p99_ms", MetricDirection.LOWER),
    MetricSpec("maximum_normal_stall_ms", MetricDirection.LOWER),
    MetricSpec("exact_search_warm_ms", MetricDirection.LOWER),
    MetricSpec("exact_search_cold_ms", MetricDirection.LOWER),
    MetricSpec("mutation_p95_us", MetricDirection.LOWER),
    MetricSpec("copy_throughput_mib_s", MetricDirection.HIGHER),
)
CORE_METRIC_NAMES = tuple(spec.name for spec in CORE_METRICS)
CORE_METRIC_BY_NAME = {spec.name: spec for spec in CORE_METRICS}


def _bounded_text(value: str, name: str, limit: int) -> str:
    cleaned = " ".join(str(value).split()).strip()
    if not cleaned or len(cleaned) > limit:
        raise ValueError(f"{name} must be 1..{limit} characters")
    return cleaned


def _valid_sha256(value: str) -> bool:
    return len(value) == 64 and all(ch in "0123456789abcdef" for ch in value)


def _utc_text(value: str) -> str:
    text = _bounded_text(value, "captured_at_utc", 64)
    parsed = datetime.fromisoformat(text.replace("Z", "+00:00"))
    if parsed.utcoffset() is None or parsed.utcoffset().total_seconds() != 0:
        raise ValueError("captured_at_utc must be UTC")
    return text


@dataclass(frozen=True)
class LabSystemState:
    os_name: str
    os_release: str
    architecture: str
    cpu_model: str
    physical_ram_mib: int
    thermal_state: str
    power_mode: str

    def validate(self) -> None:
        _bounded_text(self.os_name, "os_name", 128)
        _bounded_text(self.os_release, "os_release", 256)
        _bounded_text(self.architecture, "architecture", 128)
        _bounded_text(self.cpu_model, "cpu_model", 512)
        _bounded_text(self.thermal_state, "thermal_state", 64)
        _bounded_text(self.power_mode, "power_mode", 64)
        if self.physical_ram_mib < 256:
            raise ValueError("physical_ram_mib is implausibly small")

    def comparison_key(self) -> tuple[object, ...]:
        self.validate()
        return (
            self.os_name,
            self.os_release,
            self.architecture,
            self.cpu_model,
            self.physical_ram_mib,
            self.thermal_state,
            self.power_mode,
        )

    def to_dict(self) -> dict[str, object]:
        self.validate()
        return {
            "os_name": self.os_name,
            "os_release": self.os_release,
            "architecture": self.architecture,
            "cpu_model": self.cpu_model,
            "physical_ram_mib": self.physical_ram_mib,
            "thermal_state": self.thermal_state,
            "power_mode": self.power_mode,
        }


@dataclass(frozen=True)
class CompetitorRun:
    engine: Engine
    engine_version: str
    corpus_sha256: str
    corpus_logical_bytes: int
    captured_at_utc: str
    run_index: int
    system_state: LabSystemState
    metrics: Mapping[str, float]
    failure_mode: str | None = None

    def validate(self) -> None:
        _bounded_text(self.engine_version, "engine_version", 256)
        if not _valid_sha256(self.corpus_sha256):
            raise ValueError("corpus_sha256 must be lowercase hexadecimal SHA-256")
        if self.corpus_logical_bytes <= 0:
            raise ValueError("corpus_logical_bytes must be positive")
        _utc_text(self.captured_at_utc)
        if self.run_index < 0:
            raise ValueError("run_index must be non-negative")
        self.system_state.validate()
        if self.failure_mode is not None:
            _bounded_text(self.failure_mode, "failure_mode", 1024)
        metric_names = set(self.metrics)
        if self.succeeded:
            if metric_names != set(CORE_METRIC_NAMES):
                raise ValueError(
                    "successful run metric set does not match the canonical core metrics"
                )
        elif metric_names and metric_names != set(CORE_METRIC_NAMES):
            raise ValueError("failed run metrics must be empty or complete")
        for name, raw in self.metrics.items():
            value = float(raw)
            if not math.isfinite(value) or value <= 0.0:
                raise ValueError(f"{name} must be finite and positive")

    @property
    def succeeded(self) -> bool:
        return self.failure_mode is None

    def to_dict(self) -> dict[str, object]:
        self.validate()
        return {
            "engine": self.engine.value,
            "engine_version": self.engine_version,
            "corpus_sha256": self.corpus_sha256,
            "corpus_logical_bytes": self.corpus_logical_bytes,
            "captured_at_utc": self.captured_at_utc,
            "run_index": self.run_index,
            "system_state": self.system_state.to_dict(),
            "metrics": {
                name: float(self.metrics[name])
                for name in CORE_METRIC_NAMES
                if name in self.metrics
            },
            "failure_mode": self.failure_mode,
        }


@dataclass(frozen=True)
class MetricAggregate:
    median: float
    p95: float
    p99: float

    def to_dict(self) -> dict[str, float]:
        return {"median": self.median, "p95": self.p95, "p99": self.p99}


@dataclass(frozen=True)
class EngineAggregate:
    engine: Engine
    engine_version: str
    corpus_sha256: str
    corpus_logical_bytes: int
    system_state: LabSystemState
    successful_runs: int
    failed_runs: int
    metrics: Mapping[str, MetricAggregate]

    def to_dict(self) -> dict[str, object]:
        return {
            "engine": self.engine.value,
            "engine_version": self.engine_version,
            "corpus_sha256": self.corpus_sha256,
            "corpus_logical_bytes": self.corpus_logical_bytes,
            "system_state": self.system_state.to_dict(),
            "successful_runs": self.successful_runs,
            "failed_runs": self.failed_runs,
            "metrics": {
                name: self.metrics[name].to_dict()
                for name in CORE_METRIC_NAMES
            },
        }


@dataclass(frozen=True)
class EngineCampaignStatus:
    status: EngineStatus
    reason: str

    def validate(self) -> None:
        if self.status == EngineStatus.UNSUPPORTED:
            _bounded_text(self.reason, "unsupported reason", 512)
        elif self.status in {EngineStatus.FAILED, EngineStatus.MISSING}:
            _bounded_text(self.reason, "failure/missing reason", 512)
        elif self.reason:
            _bounded_text(self.reason, "status reason", 512)

    def to_dict(self) -> dict[str, str]:
        self.validate()
        return {"status": self.status.value, "reason": self.reason}


def nearest_rank(values: Sequence[float], percentile: float) -> float:
    if not values:
        raise ValueError("percentile values must not be empty")
    if not (0.0 < percentile <= 100.0):
        raise ValueError("percentile must be in (0, 100]")
    ordered = sorted(float(value) for value in values)
    if any(not math.isfinite(value) for value in ordered):
        raise ValueError("percentile values must be finite")
    rank = max(1, math.ceil(percentile / 100.0 * len(ordered)))
    return ordered[rank - 1]


def aggregate_engine_runs(runs: Sequence[CompetitorRun]) -> EngineAggregate:
    if not runs:
        raise ValueError("engine run set must not be empty")
    for run in runs:
        run.validate()
    engine = runs[0].engine
    version = runs[0].engine_version
    corpus_sha = runs[0].corpus_sha256
    corpus_bytes = runs[0].corpus_logical_bytes
    system_key = runs[0].system_state.comparison_key()
    seen_indices: set[int] = set()
    for run in runs:
        if run.engine != engine or run.engine_version != version:
            raise ValueError("engine aggregate mixes engine identities or versions")
        if run.corpus_sha256 != corpus_sha or run.corpus_logical_bytes != corpus_bytes:
            raise ValueError("engine aggregate mixes corpus identities")
        if run.system_state.comparison_key() != system_key:
            raise ValueError("engine aggregate mixes incomparable system states")
        if run.run_index in seen_indices:
            raise ValueError("duplicate run_index in engine aggregate")
        seen_indices.add(run.run_index)

    successful = [run for run in runs if run.succeeded]
    failed = len(runs) - len(successful)
    if len(successful) < MIN_SUCCESSFUL_RUNS_PER_ENGINE:
        raise ValueError("insufficient successful runs for engine aggregate")

    metrics: dict[str, MetricAggregate] = {}
    for name in CORE_METRIC_NAMES:
        samples = [float(run.metrics[name]) for run in successful]
        metrics[name] = MetricAggregate(
            median=float(statistics.median(samples)),
            p95=nearest_rank(samples, 95.0),
            p99=nearest_rank(samples, 99.0),
        )
    return EngineAggregate(
        engine,
        version,
        corpus_sha,
        corpus_bytes,
        runs[0].system_state,
        len(successful),
        failed,
        metrics,
    )


def _within_gap(value: float, leader: float, direction: MetricDirection) -> bool:
    if direction == MetricDirection.LOWER:
        return value <= leader * (1.0 + LEADERSHIP_MAX_GAP_FRACTION)
    return value >= leader * (1.0 - LEADERSHIP_MAX_GAP_FRACTION)


def _is_first(value: float, leader: float, direction: MetricDirection) -> bool:
    tolerance = max(abs(leader), 1.0) * 1e-12
    if direction == MetricDirection.LOWER:
        return value <= leader + tolerance
    return value + tolerance >= leader


def evaluate_leadership(
    aggregates: Mapping[Engine, EngineAggregate],
    statuses: Mapping[Engine, EngineCampaignStatus],
) -> dict[str, object]:
    if set(statuses) != set(Engine):
        raise ValueError("campaign statuses must cover every canonical engine")
    for status in statuses.values():
        status.validate()
    if Engine.ZEVRYON not in aggregates:
        raise ValueError("Zevryon aggregate is required")
    if statuses[Engine.ZEVRYON].status != EngineStatus.MEASURED:
        raise ValueError("Zevryon status must be measured")
    for engine in Engine:
        if statuses[engine].status == EngineStatus.MEASURED and engine not in aggregates:
            raise ValueError(f"measured engine {engine.value} has no aggregate")
    for engine, aggregate in aggregates.items():
        if aggregate.engine != engine:
            raise ValueError("aggregate key does not match engine")
        if statuses[engine].status != EngineStatus.MEASURED:
            raise ValueError("aggregate exists for non-measured engine")
        if aggregate.successful_runs < MIN_SUCCESSFUL_RUNS_PER_ENGINE:
            raise ValueError("aggregate has insufficient successful runs")

    measured_competitors = [
        engine
        for engine in Engine
        if engine != Engine.ZEVRYON
        and statuses[engine].status == EngineStatus.MEASURED
    ]
    blockers: list[str] = []
    if len(measured_competitors) < MIN_MEASURED_COMPETITORS_FOR_LEADERSHIP:
        blockers.append("too_few_measured_competitors")
    for engine in Engine:
        if engine == Engine.ZEVRYON:
            continue
        status = statuses[engine].status
        if status in {EngineStatus.FAILED, EngineStatus.MISSING}:
            blockers.append(f"{engine.value}_{status.value}")

    zevryon = aggregates[Engine.ZEVRYON]
    per_metric: dict[str, dict[str, object]] = {}
    first_count = 0
    all_within_gap = True
    measured_engines = [
        engine for engine in Engine if statuses[engine].status == EngineStatus.MEASURED
    ]
    for spec in CORE_METRICS:
        values = {
            engine: aggregates[engine].metrics[spec.name].median
            for engine in measured_engines
        }
        leader = (
            min(values.values())
            if spec.direction == MetricDirection.LOWER
            else max(values.values())
        )
        value = zevryon.metrics[spec.name].median
        first = _is_first(value, leader, spec.direction)
        within = _within_gap(value, leader, spec.direction)
        if first:
            first_count += 1
        if not within:
            all_within_gap = False
        leaders = [
            engine.value
            for engine, candidate in values.items()
            if _is_first(candidate, leader, spec.direction)
        ]
        per_metric[spec.name] = {
            "direction": spec.direction.value,
            "zevryon_median": value,
            "leader_median": leader,
            "leaders": leaders,
            "zevryon_first": first,
            "within_five_percent": within,
        }

    measured_aggregates = [aggregates[engine] for engine in measured_engines]
    zevryon_context = (
        zevryon.corpus_sha256,
        zevryon.corpus_logical_bytes,
        zevryon.system_state.comparison_key(),
    )
    same_corpus_and_system = all(
        (
            aggregate.corpus_sha256,
            aggregate.corpus_logical_bytes,
            aggregate.system_state.comparison_key(),
        )
        == zevryon_context
        for aggregate in measured_aggregates
    )
    no_failed_runs = all(
        aggregate.failed_runs == 0 for aggregate in measured_aggregates
    )

    checks = {
        "all_canonical_statuses_declared": True,
        "same_corpus_and_comparable_system_state": same_corpus_and_system,
        "no_failed_benchmark_runs": no_failed_runs,
        "minimum_measured_competitors": (
            len(measured_competitors) >= MIN_MEASURED_COMPETITORS_FOR_LEADERSHIP
        ),
        "no_failed_or_missing_competitors": not any(
            statuses[engine].status in {EngineStatus.FAILED, EngineStatus.MISSING}
            for engine in Engine
            if engine != Engine.ZEVRYON
        ),
        "first_in_at_least_four_core_metrics": (
            first_count >= LEADERSHIP_MIN_FIRST_METRICS
        ),
        "within_five_percent_of_leader_everywhere_else": all_within_gap,
    }
    allowed = all(checks.values())
    return {
        "leadership_claim_allowed": allowed,
        "first_metric_count": first_count,
        "measured_competitor_count": len(measured_competitors),
        "checks": checks,
        "blockers": sorted(set(blockers)),
        "metrics": per_metric,
    }


def campaign_sha256(runs: Iterable[CompetitorRun]) -> str:
    ordered = sorted(
        runs,
        key=lambda run: (run.engine.value, run.run_index, run.captured_at_utc),
    )
    payload = [run.to_dict() for run in ordered]
    canonical = json.dumps(
        payload, sort_keys=True, separators=(",", ":")
    ).encode("utf-8")
    return hashlib.sha256(canonical).hexdigest()


def _expect_mapping(value: object, name: str) -> Mapping[str, object]:
    if not isinstance(value, Mapping):
        raise ValueError(f"{name} must be an object")
    return value


def system_state_from_mapping(value: Mapping[str, object]) -> LabSystemState:
    required = {
        "os_name",
        "os_release",
        "architecture",
        "cpu_model",
        "physical_ram_mib",
        "thermal_state",
        "power_mode",
    }
    if set(value) != required:
        raise ValueError("system_state fields mismatch")
    ram = value["physical_ram_mib"]
    if not isinstance(ram, int) or isinstance(ram, bool):
        raise ValueError("physical_ram_mib must be an integer")
    state = LabSystemState(
        os_name=str(value["os_name"]),
        os_release=str(value["os_release"]),
        architecture=str(value["architecture"]),
        cpu_model=str(value["cpu_model"]),
        physical_ram_mib=ram,
        thermal_state=str(value["thermal_state"]),
        power_mode=str(value["power_mode"]),
    )
    state.validate()
    return state


def run_from_mapping(value: Mapping[str, object]) -> CompetitorRun:
    required = {
        "engine",
        "engine_version",
        "corpus_sha256",
        "corpus_logical_bytes",
        "captured_at_utc",
        "run_index",
        "system_state",
        "metrics",
        "failure_mode",
    }
    if set(value) != required:
        raise ValueError("competitor run fields mismatch")
    logical_bytes = value["corpus_logical_bytes"]
    run_index = value["run_index"]
    if not isinstance(logical_bytes, int) or isinstance(logical_bytes, bool):
        raise ValueError("corpus_logical_bytes must be an integer")
    if not isinstance(run_index, int) or isinstance(run_index, bool):
        raise ValueError("run_index must be an integer")
    metrics_raw = _expect_mapping(value["metrics"], "metrics")
    metrics = {str(name): float(raw) for name, raw in metrics_raw.items()}
    failure_mode = value["failure_mode"]
    if failure_mode is not None and not isinstance(failure_mode, str):
        raise ValueError("failure_mode must be a string or null")
    run = CompetitorRun(
        engine=Engine(str(value["engine"])),
        engine_version=str(value["engine_version"]),
        corpus_sha256=str(value["corpus_sha256"]),
        corpus_logical_bytes=logical_bytes,
        captured_at_utc=str(value["captured_at_utc"]),
        run_index=run_index,
        system_state=system_state_from_mapping(
            _expect_mapping(value["system_state"], "system_state")
        ),
        metrics=metrics,
        failure_mode=failure_mode,
    )
    run.validate()
    return run


def status_from_mapping(value: Mapping[str, object]) -> EngineCampaignStatus:
    if set(value) != {"status", "reason"}:
        raise ValueError("engine status fields mismatch")
    status = EngineCampaignStatus(
        status=EngineStatus(str(value["status"])),
        reason=str(value["reason"]),
    )
    status.validate()
    return status


def evaluate_campaign_payload(payload: Mapping[str, object]) -> dict[str, object]:
    if set(payload) != {"schema_version", "statuses", "runs"}:
        raise ValueError("competitor campaign fields mismatch")
    if payload["schema_version"] != SCHEMA_VERSION:
        raise ValueError("unsupported competitor campaign schema")

    statuses_raw = _expect_mapping(payload["statuses"], "statuses")
    if set(statuses_raw) != {engine.value for engine in Engine}:
        raise ValueError("statuses must cover every canonical engine")
    statuses = {
        engine: status_from_mapping(
            _expect_mapping(statuses_raw[engine.value], f"status {engine.value}")
        )
        for engine in Engine
    }

    runs_raw = payload["runs"]
    if not isinstance(runs_raw, list):
        raise ValueError("runs must be an array")
    runs = [run_from_mapping(_expect_mapping(raw, "run")) for raw in runs_raw]

    grouped: dict[Engine, list[CompetitorRun]] = {engine: [] for engine in Engine}
    for run in runs:
        grouped[run.engine].append(run)

    aggregates: dict[Engine, EngineAggregate] = {}
    for engine in Engine:
        status = statuses[engine].status
        engine_runs = grouped[engine]
        if status == EngineStatus.MEASURED:
            aggregates[engine] = aggregate_engine_runs(engine_runs)
        elif engine_runs and status == EngineStatus.UNSUPPORTED:
            raise ValueError("unsupported engine must not contain benchmark runs")

    leadership = evaluate_leadership(aggregates, statuses)
    failure_modes = {
        engine.value: [
            run.failure_mode
            for run in grouped[engine]
            if run.failure_mode is not None
        ]
        for engine in Engine
    }
    return {
        "schema_version": SCHEMA_VERSION,
        "campaign_sha256": campaign_sha256(runs),
        "raw_run_count": len(runs),
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
