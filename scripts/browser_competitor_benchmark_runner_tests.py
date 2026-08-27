#!/usr/bin/env python3
from __future__ import annotations

import copy

from browser_competitor_benchmark import _competitor_sets, _validate_case_result
from browser_competitor_benchmark_evidence import EvidenceIdentity, HARNESS_SCHEMA
from browser_competitor_benchmark_plan import plan_benchmark_cases
from browser_competitor_normalized_browser_lifecycle import (
    NORMALIZED_MEMORY_SCOPE,
    NORMALIZED_SETUP_BOUNDARY,
)
from browser_competitor_normalized_core_evidence import (
    build_normalized_core_evidence,
)
from browser_competitor_registry import get_spec, terminal_record


SHA_A = "a" * 64
SHA_B = "b" * 64
SHA_C = "c" * 64
SHA_D = "d" * 64


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def identity() -> EvidenceIdentity:
    return EvidenceIdentity(
        host_platform="TestOS",
        host_arch="x86_64",
        system_fingerprint=SHA_A,
        harness_schema=HARNESS_SCHEMA,
        corpus_sha256=SHA_B,
        scenario_fingerprint=SHA_C,
    )


def success_result() -> dict[str, object]:
    spec = get_spec("servo")
    evidence = build_normalized_core_evidence(
        identity(),
        setup_to_ready_seconds=0.25,
        query_samples_ms=[1.0, 2.0],
        warmup_query_count=3,
        incremental_peak_memory_mb=12.5,
        setup_boundary=NORMALIZED_SETUP_BOUNDARY,
        memory_scope=NORMALIZED_MEMORY_SCOPE,
    )
    return {
        **terminal_record(
            spec,
            "success",
            runtime_identity="servo|binary=/opt/servo|version=test",
            **identity().as_terminal_kwargs(),
        ),
        "browser": "servo",
        "mode": "virtualized",
        "payload_bytes": 4096,
        "query_count": 2,
        "warmup_query_count": 3,
        "warmup_query_details": [
            {"byte_offset": 10, "milliseconds": 0.5},
            {"byte_offset": 20, "milliseconds": 0.6},
            {"byte_offset": 30, "milliseconds": 0.7},
        ],
        "query_details": [
            {"byte_offset": 0, "milliseconds": 1.0},
            {"byte_offset": 1, "milliseconds": 2.0},
        ],
        "memory_metric_status": "valid",
        "browser_scope_peak_mb": 12.5,
        "normalized_setup_to_ready_seconds": 0.25,
        "normalized_core_evidence": evidence,
    }


def validate(result: dict[str, object]) -> dict[str, object]:
    plan = plan_benchmark_cases(["servo"], ["virtualized"])[0]
    return _validate_case_result(
        plan,
        4096,
        result,
        expected_query_count=2,
        expected_warmup_query_count=3,
    )


def main() -> int:
    valid = validate(success_result())
    require(valid["status"] == "success", "valid normalized success result was rejected")

    drifted = success_result()
    drifted["browser"] = "ladybird"
    invalid_identity = validate(drifted)
    require(
        invalid_identity["status"] == "invalid",
        "browser identity drift was accepted",
    )
    require(
        "browser" in str(invalid_identity["reason"]),
        "identity drift reason lost",
    )

    bad_memory = success_result()
    bad_memory["memory_metric_status"] = "invalid"
    invalid_memory = validate(bad_memory)
    require(
        invalid_memory["status"] == "invalid",
        "invalid memory evidence was accepted",
    )

    bad_queries = success_result()
    bad_queries["query_details"] = [
        {"byte_offset": 0, "milliseconds": 1.0}
    ]
    invalid_queries = validate(bad_queries)
    require(
        invalid_queries["status"] == "invalid",
        "inconsistent query evidence was accepted",
    )

    bad_warmups = success_result()
    bad_warmups["warmup_query_details"] = []
    invalid_warmups = validate(bad_warmups)
    require(
        invalid_warmups["status"] == "invalid",
        "inconsistent warmup evidence was accepted",
    )

    missing_evidence = success_result()
    missing_evidence.pop("scenario_fingerprint")
    invalid_terminal = validate(missing_evidence)
    require(
        invalid_terminal["status"] == "invalid",
        "missing terminal evidence was accepted",
    )

    missing_normalized = success_result()
    missing_normalized.pop("normalized_core_evidence")
    invalid_normalized = validate(missing_normalized)
    require(
        invalid_normalized["status"] == "invalid",
        "missing normalized core evidence was accepted",
    )

    normalized_identity_drift = success_result()
    normalized_identity_drift["normalized_core_evidence"] = copy.deepcopy(
        normalized_identity_drift["normalized_core_evidence"]
    )
    normalized_identity_drift["normalized_core_evidence"]["scenario_fingerprint"] = SHA_D
    invalid_normalized_identity = validate(normalized_identity_drift)
    require(
        invalid_normalized_identity["status"] == "invalid",
        "normalized identity drift was accepted",
    )

    normalized_query_drift = success_result()
    normalized_query_drift["query_details"] = [
        {"byte_offset": 0, "milliseconds": 1.0},
        {"byte_offset": 1, "milliseconds": 2.25},
    ]
    invalid_normalized_query = validate(normalized_query_drift)
    require(
        invalid_normalized_query["status"] == "invalid",
        "normalized/query-detail timing drift was accepted",
    )

    normalized_setup_drift = success_result()
    normalized_setup_drift["normalized_setup_to_ready_seconds"] = 0.5
    invalid_normalized_setup = validate(normalized_setup_drift)
    require(
        invalid_normalized_setup["status"] == "invalid",
        "normalized setup lifecycle drift was accepted",
    )

    normalized_memory_drift = success_result()
    normalized_memory_drift["browser_scope_peak_mb"] = 13.5
    invalid_normalized_memory = validate(normalized_memory_drift)
    require(
        invalid_normalized_memory["status"] == "invalid",
        "normalized process-scope memory drift was accepted",
    )

    wrong_runner_query_count = success_result()
    plan = plan_benchmark_cases(["servo"], ["virtualized"])[0]
    invalid_runner_query_count = _validate_case_result(
        plan,
        4096,
        wrong_runner_query_count,
        expected_query_count=3,
        expected_warmup_query_count=3,
    )
    require(
        invalid_runner_query_count["status"] == "invalid",
        "runner query-count authority drift was accepted",
    )

    wrong_runner_warmup_count = success_result()
    invalid_runner_warmup_count = _validate_case_result(
        plan,
        4096,
        wrong_runner_warmup_count,
        expected_query_count=2,
        expected_warmup_query_count=4,
    )
    require(
        invalid_runner_warmup_count["status"] == "invalid",
        "runner warmup-count authority drift was accepted",
    )

    unavailable = {
        **terminal_record(
            get_spec("servo"),
            "unavailable",
            reason="missing binary",
        ),
        "browser": "servo",
        "mode": "virtualized",
        "payload_bytes": 4096,
    }
    validated_unavailable = _validate_case_result(plan, 4096, unavailable)
    require(
        validated_unavailable["status"] == "unavailable",
        "valid unavailable state was rewritten",
    )

    unavailable_with_normalized = dict(unavailable)
    unavailable_with_normalized["normalized_core_evidence"] = success_result()[
        "normalized_core_evidence"
    ]
    invalid_unavailable = _validate_case_result(
        plan,
        4096,
        unavailable_with_normalized,
    )
    require(
        invalid_unavailable["status"] == "invalid",
        "non-success normalized evidence was accepted",
    )

    sets = _competitor_sets(
        ["servo", "ladybird", "chrome"],
        [
            {"competitor": "servo", "status": "success"},
            {"competitor": "servo", "status": "success"},
            {"competitor": "ladybird", "status": "unavailable"},
            {"competitor": "ladybird", "status": "unavailable"},
            {"competitor": "chrome", "status": "success"},
            {"competitor": "chrome", "status": "error"},
        ],
    )
    require(
        sets["unavailable_competitors"] == ["ladybird"],
        "unavailable set drifted",
    )
    require(
        sets["available_competitors"] == ["servo", "chrome"],
        "available set drifted",
    )
    require(
        sets["successfully_measured_competitors"] == ["servo", "chrome"],
        "successful set drifted",
    )
    require(
        sets["fully_measured_competitors"] == ["servo"],
        "fully measured set drifted",
    )

    print("Zevryon normalized competitor benchmark runner tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
