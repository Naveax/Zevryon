#!/usr/bin/env python3
from __future__ import annotations

import copy

from browser_competitor_benchmark_evidence import (
    EvidenceIdentity,
    HARNESS_SCHEMA,
    synthetic_corpus_sha256,
)
from browser_competitor_canonical_full_set import (
    CanonicalFullSetInvalid,
    validate_canonical_full_set_report,
    validate_zevryon_corpus_report,
)
from browser_competitor_normalized_browser_lifecycle import (
    NORMALIZED_MEMORY_SCOPE,
    NORMALIZED_SETUP_BOUNDARY,
)
from browser_competitor_normalized_core_evidence import (
    build_normalized_core_evidence,
)
from browser_competitor_registry import (
    CANONICAL_KEYS,
    get_spec,
    leadership_coverage,
    terminal_record,
)


PAYLOAD_BYTES = 64 * 1024 * 1024
QUERY_COUNT = 3
WARMUP_QUERY_COUNT = 2
SYSTEM_SHA = "a" * 64
VIRTUAL_SCENARIO_SHA = "b" * 64
NATIVE_SCENARIO_SHA = "c" * 64


def require_invalid(callable_, message: str) -> None:
    try:
        callable_()
    except CanonicalFullSetInvalid:
        return
    raise AssertionError(message)


def zevryon_report() -> dict[str, object]:
    expected_sha = synthetic_corpus_sha256(PAYLOAD_BYTES)
    return {
        "schema": "zevryon.massivedoc.benchmark.v4",
        "giant_record_bytes": PAYLOAD_BYTES,
        "giant_record_index": 17,
        "giant_record_profile": "m7-competitor",
        "giant_record_sha256": expected_sha,
        "giant_record_expected_m7_sha256": expected_sha,
        "giant_record_matches_m7_synthetic": True,
    }


def identity(mode: str) -> EvidenceIdentity:
    return EvidenceIdentity(
        host_platform="TestOS",
        host_arch="x86_64",
        system_fingerprint=SYSTEM_SHA,
        harness_schema=HARNESS_SCHEMA,
        corpus_sha256=synthetic_corpus_sha256(PAYLOAD_BYTES),
        scenario_fingerprint=(
            VIRTUAL_SCENARIO_SHA
            if mode == "virtualized"
            else NATIVE_SCENARIO_SHA
        ),
    )


def case(competitor: str, mode: str, ordinal: int) -> dict[str, object]:
    spec = get_spec(competitor)
    query_samples = [
        1.0 + ordinal / 10.0,
        2.0 + ordinal / 10.0,
        3.0 + ordinal / 10.0,
    ]
    evidence = build_normalized_core_evidence(
        identity(mode),
        setup_to_ready_seconds=0.5 + ordinal / 100.0,
        query_samples_ms=query_samples,
        warmup_query_count=WARMUP_QUERY_COUNT,
        incremental_peak_memory_mb=20.0 + ordinal,
        setup_boundary=NORMALIZED_SETUP_BOUNDARY,
        memory_scope=NORMALIZED_MEMORY_SCOPE,
    )
    return {
        **terminal_record(
            spec,
            "success",
            runtime_identity=f"{competitor}|test-runtime",
            **identity(mode).as_terminal_kwargs(),
        ),
        "browser": competitor,
        "mode": mode,
        "payload_bytes": PAYLOAD_BYTES,
        "query_count": QUERY_COUNT,
        "warmup_query_count": WARMUP_QUERY_COUNT,
        "normalized_core_evidence": evidence,
    }


def report() -> dict[str, object]:
    cases = [
        case(competitor, mode, ordinal)
        for ordinal, competitor in enumerate(CANONICAL_KEYS)
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


def main() -> int:
    valid_zevryon = zevryon_report()
    validate_zevryon_corpus_report(valid_zevryon, PAYLOAD_BYTES)

    require_invalid(
        lambda: validate_zevryon_corpus_report(valid_zevryon, 0),
        "non-positive canonical payload size was accepted",
    )

    wrong_schema = copy.deepcopy(valid_zevryon)
    wrong_schema["schema"] = "zevryon.massivedoc.benchmark.v3"
    require_invalid(
        lambda: validate_zevryon_corpus_report(wrong_schema, PAYLOAD_BYTES),
        "legacy Zevryon benchmark schema was accepted",
    )

    wrong_size = copy.deepcopy(valid_zevryon)
    wrong_size["giant_record_bytes"] = PAYLOAD_BYTES - 1
    require_invalid(
        lambda: validate_zevryon_corpus_report(wrong_size, PAYLOAD_BYTES),
        "giant record with wrong byte count was accepted",
    )

    legacy_profile = copy.deepcopy(valid_zevryon)
    legacy_profile["giant_record_profile"] = "legacy"
    require_invalid(
        lambda: validate_zevryon_corpus_report(legacy_profile, PAYLOAD_BYTES),
        "legacy giant-record profile was accepted",
    )

    bool_index = copy.deepcopy(valid_zevryon)
    bool_index["giant_record_index"] = True
    require_invalid(
        lambda: validate_zevryon_corpus_report(bool_index, PAYLOAD_BYTES),
        "boolean giant record index was accepted",
    )

    negative_index = copy.deepcopy(valid_zevryon)
    negative_index["giant_record_index"] = -1
    require_invalid(
        lambda: validate_zevryon_corpus_report(negative_index, PAYLOAD_BYTES),
        "negative giant record index was accepted",
    )

    wrong_sha = copy.deepcopy(valid_zevryon)
    wrong_sha["giant_record_sha256"] = "0" * 64
    require_invalid(
        lambda: validate_zevryon_corpus_report(wrong_sha, PAYLOAD_BYTES),
        "wrong giant-record SHA was accepted",
    )

    drifted_authority = copy.deepcopy(valid_zevryon)
    drifted_authority["giant_record_expected_m7_sha256"] = "f" * 64
    require_invalid(
        lambda: validate_zevryon_corpus_report(
            drifted_authority,
            PAYLOAD_BYTES,
        ),
        "drifted expected-M7 SHA authority was accepted",
    )

    no_parity = copy.deepcopy(valid_zevryon)
    no_parity["giant_record_matches_m7_synthetic"] = False
    require_invalid(
        lambda: validate_zevryon_corpus_report(no_parity, PAYLOAD_BYTES),
        "missing exact corpus parity receipt was accepted",
    )

    valid = report()
    validate_canonical_full_set_report(valid)

    missing_request = copy.deepcopy(valid)
    missing_request["requested_competitors"].pop()
    require_invalid(
        lambda: validate_canonical_full_set_report(missing_request),
        "partial canonical request set was accepted",
    )

    missing_case = copy.deepcopy(valid)
    missing_case["browser_cases"].pop()
    require_invalid(
        lambda: validate_canonical_full_set_report(missing_case),
        "partial canonical case matrix was accepted",
    )

    duplicate_case = copy.deepcopy(valid)
    duplicate_case["browser_cases"][-1] = copy.deepcopy(
        duplicate_case["browser_cases"][0]
    )
    require_invalid(
        lambda: validate_canonical_full_set_report(duplicate_case),
        "duplicate canonical case was accepted",
    )

    native_failure = copy.deepcopy(valid)
    native_case = next(
        item
        for item in native_failure["browser_cases"]
        if item["competitor"] == "ladybird"
        and item["mode"] == "native-dom"
    )
    native_case["status"] = "error"
    native_case["reason"] = "injected failure"
    native_case["runtime_identity"] = None
    native_case.pop("normalized_core_evidence")
    native_failure["all_requested_cases_succeeded"] = False
    native_failure["leadership_coverage_by_mode"]["native-dom"] = leadership_coverage(
        [
            item
            for item in native_failure["browser_cases"]
            if item["mode"] == "native-dom"
        ]
    )
    require_invalid(
        lambda: validate_canonical_full_set_report(native_failure),
        "native-DOM failure was accepted",
    )

    missing_normalized = copy.deepcopy(valid)
    missing_normalized["browser_cases"][0].pop("normalized_core_evidence")
    require_invalid(
        lambda: validate_canonical_full_set_report(missing_normalized),
        "canonical success without normalized core evidence was accepted",
    )

    terminal_normalized_drift = copy.deepcopy(valid)
    terminal_normalized_drift["browser_cases"][0][
        "system_fingerprint"
    ] = "d" * 64
    require_invalid(
        lambda: validate_canonical_full_set_report(
            terminal_normalized_drift
        ),
        "terminal/normalized identity drift was accepted",
    )

    comparison_drift = copy.deepcopy(valid)
    drift_case = next(
        item
        for item in comparison_drift["browser_cases"]
        if item["competitor"] == "edge"
        and item["mode"] == "virtualized"
    )
    drift_case["system_fingerprint"] = "e" * 64
    drift_case["normalized_core_evidence"]["system_fingerprint"] = "e" * 64
    comparison_drift["leadership_coverage_by_mode"][
        "virtualized"
    ] = leadership_coverage(
        [
            item
            for item in comparison_drift["browser_cases"]
            if item["mode"] == "virtualized"
        ]
    )
    require_invalid(
        lambda: validate_canonical_full_set_report(comparison_drift),
        "normalized comparison identity drift was accepted",
    )

    forged_coverage = copy.deepcopy(valid)
    forged_coverage["leadership_coverage_by_mode"]["virtualized"][
        "canonical_measured"
    ] = []
    require_invalid(
        lambda: validate_canonical_full_set_report(forged_coverage),
        "forged leadership coverage was accepted",
    )

    wrong_query_count = copy.deepcopy(valid)
    wrong_query_count["browser_cases"][0]["query_count"] = QUERY_COUNT + 1
    require_invalid(
        lambda: validate_canonical_full_set_report(wrong_query_count),
        "per-case query count drift was accepted",
    )

    wrong_warmup_count = copy.deepcopy(valid)
    wrong_warmup_count["browser_cases"][0][
        "warmup_query_count"
    ] = WARMUP_QUERY_COUNT + 1
    require_invalid(
        lambda: validate_canonical_full_set_report(wrong_warmup_count),
        "per-case warmup count drift was accepted",
    )

    premature_metric = copy.deepcopy(valid)
    premature_metric["leadership_metric_gate_evaluated"] = True
    require_invalid(
        lambda: validate_canonical_full_set_report(premature_metric),
        "premature metric evaluation was accepted",
    )

    premature_leadership = copy.deepcopy(valid)
    premature_leadership["leadership_eligible"] = True
    require_invalid(
        lambda: validate_canonical_full_set_report(premature_leadership),
        "premature leadership claim was accepted",
    )

    print(
        "canonical full-set normalized evidence and Zevryon corpus parity gate tests passed"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
