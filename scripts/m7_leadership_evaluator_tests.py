#!/usr/bin/env python3
from __future__ import annotations

import copy

from browser_competitor_benchmark_evidence import EvidenceIdentity, HARNESS_SCHEMA
from browser_competitor_normalized_browser_lifecycle import (
    NORMALIZED_MEMORY_SCOPE,
    NORMALIZED_SETUP_BOUNDARY,
)
from browser_competitor_normalized_core_evidence import build_normalized_core_evidence
from browser_competitor_query_plan import plan_query_offsets
from browser_competitor_registry import (
    CANONICAL_KEYS,
    get_spec,
    leadership_coverage,
    terminal_record,
)
from browser_competitor_scenario_contract import VIEWPORT_HEIGHT, VIEWPORT_WIDTH
from m7_leadership_evaluator import (
    LeadershipEvaluationInvalid,
    evaluate_leadership,
)
from m7_synthetic_corpus import synthetic_corpus_sha256
from m7_zevryon_normalized_case import (
    CASE_SCHEMA,
    M7_SOURCE_AUTHORITY,
    SESSION_SCHEMA,
)


PAYLOAD_BYTES = 4096
QUERY_COUNT = 3
WARMUP_QUERY_COUNT = 2
VIRTUAL_SLICE_BYTES = 256
SYSTEM_SHA = "a" * 64
CORPUS_SHA = synthetic_corpus_sha256(PAYLOAD_BYTES)
VIRTUAL_SCENARIO_SHA = "c" * 64
NATIVE_SCENARIO_SHA = "d" * 64


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def require_invalid(callable_, message: str) -> None:
    try:
        callable_()
    except LeadershipEvaluationInvalid:
        return
    raise AssertionError(message)


def identity(mode: str) -> EvidenceIdentity:
    return EvidenceIdentity(
        host_platform="TestOS",
        host_arch="x86_64",
        system_fingerprint=SYSTEM_SHA,
        harness_schema=HARNESS_SCHEMA,
        corpus_sha256=CORPUS_SHA,
        scenario_fingerprint=(
            VIRTUAL_SCENARIO_SHA if mode == "virtualized" else NATIVE_SCENARIO_SHA
        ),
    )


def evidence(
    mode: str,
    *,
    setup: float,
    query: float,
    memory: float,
) -> dict[str, object]:
    return build_normalized_core_evidence(
        identity(mode),
        setup_to_ready_seconds=setup,
        query_samples_ms=[query, query, query],
        warmup_query_count=WARMUP_QUERY_COUNT,
        incremental_peak_memory_mb=memory,
        setup_boundary=NORMALIZED_SETUP_BOUNDARY,
        memory_scope=NORMALIZED_MEMORY_SCOPE,
    )


def browser_case(competitor: str, mode: str) -> dict[str, object]:
    normalized = evidence(mode, setup=10.0, query=10.0, memory=10.0)
    return {
        **terminal_record(
            get_spec(competitor),
            "success",
            runtime_identity=f"{competitor}|test-runtime",
            **identity(mode).as_terminal_kwargs(),
        ),
        "browser": competitor,
        "mode": mode,
        "payload_bytes": PAYLOAD_BYTES,
        "query_count": QUERY_COUNT,
        "warmup_query_count": WARMUP_QUERY_COUNT,
        "normalized_core_evidence": normalized,
    }


def browser_report() -> dict[str, object]:
    cases = [
        browser_case(competitor, mode)
        for competitor in CANONICAL_KEYS
        for mode in ("virtualized", "native-dom")
    ]
    return {
        "schema": HARNESS_SCHEMA,
        "requested_competitors": list(CANONICAL_KEYS),
        "query_count": QUERY_COUNT,
        "warmup_query_count": WARMUP_QUERY_COUNT,
        "browser_cases": cases,
        "all_requested_cases_succeeded": True,
        "leadership_coverage_by_mode": {
            mode: leadership_coverage(
                [item for item in cases if item["mode"] == mode]
            )
            for mode in ("virtualized", "native-dom")
        },
        "leadership_metric_gate_evaluated": False,
        "leadership_eligible": False,
    }


def session_transcript(mode: str, samples: list[float]) -> tuple[dict[str, object], list[dict[str, object]], dict[str, object]]:
    plan = plan_query_offsets(
        mode=mode,
        payload_bytes=PAYLOAD_BYTES,
        virtual_slice_bytes=VIRTUAL_SLICE_BYTES,
        query_count=QUERY_COUNT,
        warmup_query_count=WARMUP_QUERY_COUNT,
    )
    ready = {
        "schema": SESSION_SCHEMA,
        "event": "ready",
        "mode": mode,
        "payload_bytes": PAYLOAD_BYTES,
        "query_count": QUERY_COUNT,
        "warmup_query_count": WARMUP_QUERY_COUNT,
        "virtual_slice_bytes": VIRTUAL_SLICE_BYTES,
        "viewport_width_px": VIEWPORT_WIDTH,
        "viewport_height_px": VIEWPORT_HEIGHT,
        "native_total_height_q8": 100 if mode == "native-dom" else 0,
        "native_checkpoint_bytes": 200 if mode == "native-dom" else 0,
        "internal_setup_seconds": 0.125,
        "source_authority": M7_SOURCE_AUTHORITY,
        "record_index": 0,
        "store_payload_sha256": CORPUS_SHA,
        "store_physical_bytes": 8192,
        "normalized_leadership_evidence": False,
    }
    details: list[dict[str, object]] = []
    coordinate_field = "byte_offset" if mode == "virtualized" else "scroll_fraction_ppm"
    for ordinal, (coordinate, sample) in enumerate(zip(plan.measured_offsets, samples)):
        details.append(
            {
                "schema": SESSION_SCHEMA,
                "event": "query",
                "ordinal": ordinal,
                coordinate_field: coordinate,
                "milliseconds": sample,
                "source_bytes_read": VIRTUAL_SLICE_BYTES,
                "rendered_height_q8": 300,
                "checkpoint_source_offset": 0 if ordinal == 0 else 128,
                "fragment_count": 4,
                "truncated": False,
            }
        )
    complete = {
        "schema": SESSION_SCHEMA,
        "event": "complete",
        "query_count": QUERY_COUNT,
        "normalized_leadership_evidence": False,
    }
    return ready, details, complete


def zevryon_case(
    mode: str,
    *,
    setup: float = 9.0,
    query: float = 9.0,
    memory: float = 10.4,
) -> dict[str, object]:
    normalized = evidence(mode, setup=setup, query=query, memory=memory)
    samples = [float(value) for value in normalized["query_samples_ms"]]
    ready, details, complete = session_transcript(mode, samples)
    return {
        "schema": CASE_SCHEMA,
        "implementation": "zevryon",
        "status": "success",
        "mode": mode,
        "payload_bytes": PAYLOAD_BYTES,
        "query_count": QUERY_COUNT,
        "warmup_query_count": WARMUP_QUERY_COUNT,
        "virtual_slice_bytes": VIRTUAL_SLICE_BYTES if mode == "virtualized" else None,
        "timeout_seconds": 180 if mode == "virtualized" else 420,
        "source_authority": M7_SOURCE_AUTHORITY,
        "record_index": 0,
        "runtime_identity": "zevryon|persistent-session|sha256=" + "e" * 64,
        **identity(mode).as_terminal_kwargs(),
        "normalized_setup_to_ready_seconds": setup,
        "process_scope_peak_mb": memory,
        "session_ready": ready,
        "query_details": details,
        "session_complete": complete,
        "normalized_core_evidence": normalized,
    }


def main() -> int:
    browsers = browser_report()
    virtual = zevryon_case("virtualized")
    native = zevryon_case("native-dom")

    result = evaluate_leadership(browsers, virtual, native)
    require(result["leadership_metric_gate_evaluated"] is True, "metric gate not evaluated")
    require(result["leadership_eligible"] is True, "valid 4/5 leadership result was rejected")
    for mode in ("virtualized", "native-dom"):
        mode_result = result["modes"][mode]
        require(mode_result["zevryon_first_count"] == 4, f"{mode} first count drifted")
        require(mode_result["leadership_eligible"] is True, f"{mode} eligibility drifted")
        require(
            mode_result["metrics"]["incremental_peak_memory_mb"]["zevryon_status"]
            == "within_5_percent",
            f"{mode} within-5 classification drifted",
        )

    reordered = copy.deepcopy(browsers)
    reordered["browser_cases"] = list(reversed(reordered["browser_cases"]))
    reordered_result = evaluate_leadership(reordered, virtual, native)
    require(
        reordered_result["leadership_eligible"] is True,
        "valid reordered canonical case matrix was rejected",
    )

    outside = zevryon_case("native-dom", memory=10.6)
    outside_result = evaluate_leadership(browsers, virtual, outside)
    require(
        outside_result["modes"]["native-dom"]["leadership_eligible"] is False,
        "metric more than 5 percent above leader was accepted",
    )
    require(
        outside_result["leadership_eligible"] is False,
        "overall leadership ignored a failing canonical mode",
    )

    only_three = zevryon_case(
        "native-dom",
        setup=10.2,
        query=9.0,
        memory=10.2,
    )
    only_three_result = evaluate_leadership(browsers, virtual, only_three)
    require(
        only_three_result["modes"]["native-dom"]["zevryon_first_count"] == 3,
        "three-first fixture drifted",
    )
    require(
        only_three_result["modes"]["native-dom"]["leadership_eligible"] is False,
        "three first metrics incorrectly satisfied the four-of-five rule",
    )

    identity_drift = zevryon_case("native-dom")
    identity_drift["normalized_core_evidence"] = copy.deepcopy(
        identity_drift["normalized_core_evidence"]
    )
    identity_drift["normalized_core_evidence"]["scenario_fingerprint"] = "f" * 64
    identity_drift["scenario_fingerprint"] = "f" * 64
    require_invalid(
        lambda: evaluate_leadership(browsers, virtual, identity_drift),
        "Zevryon/browser scenario identity drift was accepted",
    )

    tampered_query = zevryon_case("native-dom")
    tampered_query["query_details"][0]["milliseconds"] = 99.0
    require_invalid(
        lambda: evaluate_leadership(browsers, virtual, tampered_query),
        "tampered Zevryon query detail was accepted",
    )

    tampered_coordinate = zevryon_case("virtualized")
    tampered_coordinate["query_details"][0]["byte_offset"] = 0
    require_invalid(
        lambda: evaluate_leadership(browsers, tampered_coordinate, native),
        "tampered deterministic Zevryon query coordinate was accepted",
    )

    prebuilt = zevryon_case("native-dom")
    prebuilt["source_authority"] = "prebuilt-store-diagnostic-v1"
    require_invalid(
        lambda: evaluate_leadership(browsers, virtual, prebuilt),
        "prebuilt Zevryon store was accepted by the leadership evaluator",
    )

    zero_browsers = browser_report()
    for case in zero_browsers["browser_cases"]:
        case["normalized_core_evidence"] = evidence(
            str(case["mode"]),
            setup=0.0,
            query=10.0,
            memory=10.0,
        )
    require_invalid(
        lambda: evaluate_leadership(zero_browsers, virtual, native),
        "zero setup leader was accepted as physically meaningful",
    )

    print("M7 five-metric leadership evaluator tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
