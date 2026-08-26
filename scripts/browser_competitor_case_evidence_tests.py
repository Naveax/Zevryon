#!/usr/bin/env python3
from __future__ import annotations

import copy
import math

from browser_competitor_case_evidence import (
    BenchmarkScenario,
    CORE_METRIC_KEYS,
    SystemState,
    build_competitor_success_record,
    build_measurement_evidence,
    canonical_fingerprint,
    validate_measurement_evidence,
)
from browser_competitor_registry import get_spec, validate_terminal_record


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def require_value_error(callable_, message: str) -> None:
    try:
        callable_()
    except ValueError:
        return
    raise AssertionError(message)


def system_state() -> SystemState:
    return SystemState(
        platform="Linux",
        arch="x86_64",
        os_version="test-os-1",
        kernel="test-kernel-1",
        cpu_model="test-cpu",
        logical_cpus=16,
        total_memory_bytes=32 * 1024 * 1024 * 1024,
        graphics_identity="test-gpu",
        graphics_driver="test-driver",
        power_mode="performance",
        thermal_state="nominal",
    )


def scenario() -> BenchmarkScenario:
    return BenchmarkScenario(
        mode="virtualized",
        payload_bytes=64 * 1024 * 1024,
        measured_query_count=5,
        warmup_query_count=2,
        virtual_slice_bytes=128 * 1024,
        viewport_width=800,
        viewport_height=720,
        deterministic_sequence_id="lcg-243f6a88-v1",
        unicode_generator_schema="emoji-pattern-v1",
        setup_boundary="cold-case-start-to-first-measured-query-ready-v1",
        memory_scope="owned-process-tree-above-harness-baseline-v1",
        timeout_seconds=180,
    )


def main() -> int:
    state = system_state()
    case = scenario()
    samples = [1.0, 2.0, 3.0, 4.0, 5.0]
    corpus = "a" * 64

    evidence = build_measurement_evidence(
        system_state=state,
        harness_schema="zevryon.competitor.harness.v2",
        corpus_sha256=corpus,
        scenario=case,
        setup_to_ready_seconds=1.25,
        query_samples_ms=samples,
        incremental_peak_memory_mb=42.5,
    )
    validate_measurement_evidence(evidence)
    require(len(str(evidence["system_fingerprint"])) == 64, "system fingerprint length mismatch")
    require(len(str(evidence["scenario_fingerprint"])) == 64, "scenario fingerprint length mismatch")
    require(evidence["query_sample_count"] == 5, "sample count mismatch")
    metrics = evidence["core_metrics"]
    require(isinstance(metrics, dict), "core metrics payload is not a dictionary")
    require(set(metrics) == set(CORE_METRIC_KEYS), "core metric keys drifted")
    require(metrics["setup_to_ready_seconds"] == 1.25, "setup metric mismatch")
    require(metrics["query_milliseconds_p50"] == 3.0, "P50 mismatch")
    require(math.isclose(metrics["query_milliseconds_p95"], 4.8), "P95 mismatch")
    require(math.isclose(metrics["query_milliseconds_p99"], 4.96), "P99 mismatch")
    require(metrics["incremental_peak_memory_mb"] == 42.5, "memory metric mismatch")

    chrome_record = build_competitor_success_record(
        get_spec("chrome"),
        runtime_identity=(
            "Google Chrome; adapter=playwright; browser_type=chromium; "
            "channel=chrome; distribution=branded-channel; version=123.4-test"
        ),
        evidence=evidence,
    )
    validate_terminal_record(chrome_record)
    validate_measurement_evidence(chrome_record)
    require(chrome_record["status"] == "success", "competitor success state mismatch")
    require(chrome_record["competitor"] == "chrome", "competitor identity mismatch")
    require(chrome_record["system_fingerprint"] == evidence["system_fingerprint"], "system fingerprint was not bound")
    require(chrome_record["scenario_fingerprint"] == evidence["scenario_fingerprint"], "scenario fingerprint was not bound")

    reordered = {
        "kernel": "test-kernel-1",
        "platform": "Linux",
        "arch": "x86_64",
    }
    differently_ordered = {
        "arch": "x86_64",
        "platform": "Linux",
        "kernel": "test-kernel-1",
    }
    require(
        canonical_fingerprint(reordered) == canonical_fingerprint(differently_ordered),
        "canonical fingerprint depends on dictionary insertion order",
    )

    tampered_metric = copy.deepcopy(evidence)
    tampered_metric["core_metrics"]["query_milliseconds_p95"] = 4.7
    require_value_error(
        lambda: validate_measurement_evidence(tampered_metric),
        "tampered percentile was accepted",
    )

    tampered_system = copy.deepcopy(evidence)
    tampered_system["system_state"]["cpu_model"] = "different-cpu"
    require_value_error(
        lambda: validate_measurement_evidence(tampered_system),
        "system-state tampering bypassed fingerprint validation",
    )

    tampered_scenario = copy.deepcopy(evidence)
    tampered_scenario["scenario"]["timeout_seconds"] = 181
    require_value_error(
        lambda: validate_measurement_evidence(tampered_scenario),
        "scenario tampering bypassed fingerprint validation",
    )

    missing_sample = copy.deepcopy(evidence)
    missing_sample["query_samples_ms"].pop()
    require_value_error(
        lambda: validate_measurement_evidence(missing_sample),
        "raw sample removal was accepted",
    )

    require_value_error(
        lambda: build_measurement_evidence(
            system_state=state,
            harness_schema="zevryon.competitor.harness.v2",
            corpus_sha256="A" * 64,
            scenario=case,
            setup_to_ready_seconds=1.0,
            query_samples_ms=samples,
            incremental_peak_memory_mb=1.0,
        ),
        "uppercase corpus hash was accepted",
    )
    require_value_error(
        lambda: build_measurement_evidence(
            system_state=state,
            harness_schema="zevryon.competitor.harness.v2",
            corpus_sha256=corpus,
            scenario=case,
            setup_to_ready_seconds=1.0,
            query_samples_ms=[1.0, 2.0],
            incremental_peak_memory_mb=1.0,
        ),
        "sample count inconsistent with scenario was accepted",
    )
    require_value_error(
        lambda: build_measurement_evidence(
            system_state=state,
            harness_schema="zevryon.competitor.harness.v2",
            corpus_sha256=corpus,
            scenario=case,
            setup_to_ready_seconds=1.0,
            query_samples_ms=[1.0, 2.0, 3.0, 4.0, float("nan")],
            incremental_peak_memory_mb=1.0,
        ),
        "NaN query sample was accepted",
    )
    require_value_error(
        lambda: build_competitor_success_record(
            get_spec("chrome"), runtime_identity="   ", evidence=evidence
        ),
        "blank runtime identity was accepted",
    )

    require_value_error(
        lambda: BenchmarkScenario(
            mode="native-dom",
            payload_bytes=1,
            measured_query_count=1,
            warmup_query_count=0,
            virtual_slice_bytes=128,
            viewport_width=1,
            viewport_height=1,
            deterministic_sequence_id="x",
            unicode_generator_schema="x",
            setup_boundary="x",
            memory_scope="x",
            timeout_seconds=1,
        ).validate(),
        "native-dom scenario accepted virtual_slice_bytes",
    )
    require_value_error(
        lambda: BenchmarkScenario(
            mode="virtualized",
            payload_bytes=1,
            measured_query_count=1,
            warmup_query_count=0,
            virtual_slice_bytes=None,
            viewport_width=1,
            viewport_height=1,
            deterministic_sequence_id="x",
            unicode_generator_schema="x",
            setup_boundary="x",
            memory_scope="x",
            timeout_seconds=1,
        ).validate(),
        "virtualized scenario accepted missing virtual_slice_bytes",
    )

    print("Zevryon normalized competitor case evidence tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
