#!/usr/bin/env python3
from __future__ import annotations

import copy

from browser_competitor_benchmark_evidence import EvidenceIdentity, HARNESS_SCHEMA
from browser_competitor_normalized_browser_lifecycle import (
    NORMALIZED_MEMORY_SCOPE,
    NORMALIZED_SETUP_BOUNDARY,
)
from browser_competitor_normalized_core_evidence import build_normalized_core_evidence
from browser_competitor_registry import (
    CANONICAL_KEYS,
    get_spec,
    leadership_coverage,
    terminal_record,
)
from m7_normalized_browser_full_set import (
    COLLECTION_AUTHORITY,
    CanonicalNormalizedBrowserSetInvalid,
    validate_canonical_normalized_browser_report,
)
from m7_synthetic_corpus import synthetic_corpus_sha256


PAYLOAD_BYTES = 4096
QUERY_COUNT = 3
WARMUP_QUERY_COUNT = 2
VIRTUAL_SLICE_BYTES = 256
SYSTEM_SHA = "a" * 64
VIRTUAL_SCENARIO_SHA = "b" * 64
NATIVE_SCENARIO_SHA = "c" * 64


def require_invalid(callable_, message: str) -> None:
    try:
        callable_()
    except CanonicalNormalizedBrowserSetInvalid:
        return
    raise AssertionError(message)


def identity(mode: str) -> EvidenceIdentity:
    return EvidenceIdentity(
        host_platform="TestOS",
        host_arch="x86_64",
        system_fingerprint=SYSTEM_SHA,
        harness_schema=HARNESS_SCHEMA,
        corpus_sha256=synthetic_corpus_sha256(PAYLOAD_BYTES),
        scenario_fingerprint=(
            VIRTUAL_SCENARIO_SHA if mode == "virtualized" else NATIVE_SCENARIO_SHA
        ),
    )


def case(competitor: str, mode: str, ordinal: int) -> dict[str, object]:
    spec = get_spec(competitor)
    evidence = build_normalized_core_evidence(
        identity(mode),
        setup_to_ready_seconds=1.0 + ordinal / 10.0,
        query_samples_ms=[1.0, 2.0, 3.0],
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
        "collection_authority": COLLECTION_AUTHORITY,
        "corpus_sha256": synthetic_corpus_sha256(PAYLOAD_BYTES),
        "payload_bytes": PAYLOAD_BYTES,
        "query_count": QUERY_COUNT,
        "warmup_query_count": WARMUP_QUERY_COUNT,
        "virtual_slice_bytes": VIRTUAL_SLICE_BYTES,
        "requested_competitors": list(CANONICAL_KEYS),
        "browser_cases": cases,
        "leadership_coverage_by_mode": {
            mode: leadership_coverage(
                [item for item in cases if item["mode"] == mode]
            )
            for mode in ("virtualized", "native-dom")
        },
        "all_requested_cases_succeeded": True,
        "leadership_metric_gate_evaluated": False,
        "leadership_eligible": False,
    }


def main() -> int:
    valid = report()
    validate_canonical_normalized_browser_report(valid)

    reordered = copy.deepcopy(valid)
    reordered["browser_cases"] = list(reversed(reordered["browser_cases"]))
    validate_canonical_normalized_browser_report(reordered)

    wrong_authority = copy.deepcopy(valid)
    wrong_authority["collection_authority"] = "legacy"
    require_invalid(
        lambda: validate_canonical_normalized_browser_report(wrong_authority),
        "wrong collection authority was accepted",
    )

    wrong_top_sha = copy.deepcopy(valid)
    wrong_top_sha["corpus_sha256"] = "0" * 64
    require_invalid(
        lambda: validate_canonical_normalized_browser_report(wrong_top_sha),
        "wrong top-level corpus SHA was accepted",
    )

    wrong_payload = copy.deepcopy(valid)
    wrong_payload["payload_bytes"] = PAYLOAD_BYTES + 1
    require_invalid(
        lambda: validate_canonical_normalized_browser_report(wrong_payload),
        "top-level payload size drift was accepted",
    )

    case_payload_drift = copy.deepcopy(valid)
    case_payload_drift["browser_cases"][0]["payload_bytes"] = PAYLOAD_BYTES + 1
    require_invalid(
        lambda: validate_canonical_normalized_browser_report(case_payload_drift),
        "per-case payload drift was accepted",
    )

    terminal_corpus_drift = copy.deepcopy(valid)
    terminal_corpus_drift["browser_cases"][0]["corpus_sha256"] = "1" * 64
    require_invalid(
        lambda: validate_canonical_normalized_browser_report(terminal_corpus_drift),
        "terminal corpus SHA drift was accepted",
    )

    normalized_corpus_drift = copy.deepcopy(valid)
    normalized_corpus_drift["browser_cases"][0]["normalized_core_evidence"] = copy.deepcopy(
        normalized_corpus_drift["browser_cases"][0]["normalized_core_evidence"]
    )
    normalized_corpus_drift["browser_cases"][0]["normalized_core_evidence"][
        "corpus_sha256"
    ] = "2" * 64
    require_invalid(
        lambda: validate_canonical_normalized_browser_report(normalized_corpus_drift),
        "normalized corpus SHA drift was accepted",
    )

    missing_case = copy.deepcopy(valid)
    missing_case["browser_cases"].pop()
    require_invalid(
        lambda: validate_canonical_normalized_browser_report(missing_case),
        "partial 6x2 canonical matrix was accepted",
    )

    duplicate_case = copy.deepcopy(valid)
    duplicate_case["browser_cases"][-1] = copy.deepcopy(
        duplicate_case["browser_cases"][0]
    )
    require_invalid(
        lambda: validate_canonical_normalized_browser_report(duplicate_case),
        "duplicate canonical case was accepted",
    )

    failed_case = copy.deepcopy(valid)
    target = failed_case["browser_cases"][0]
    target["status"] = "unavailable"
    target["reason"] = "missing exact runtime"
    target["runtime_identity"] = None
    target.pop("normalized_core_evidence")
    failed_case["all_requested_cases_succeeded"] = False
    failed_case["leadership_coverage_by_mode"][str(target["mode"])] = leadership_coverage(
        [
            item
            for item in failed_case["browser_cases"]
            if item["mode"] == target["mode"]
        ]
    )
    require_invalid(
        lambda: validate_canonical_normalized_browser_report(failed_case),
        "unavailable canonical runtime was accepted",
    )

    forged_coverage = copy.deepcopy(valid)
    forged_coverage["leadership_coverage_by_mode"]["virtualized"][
        "canonical_measured"
    ] = []
    require_invalid(
        lambda: validate_canonical_normalized_browser_report(forged_coverage),
        "forged coverage was accepted",
    )

    premature_metric = copy.deepcopy(valid)
    premature_metric["leadership_metric_gate_evaluated"] = True
    require_invalid(
        lambda: validate_canonical_normalized_browser_report(premature_metric),
        "browser collector evaluated the metric gate",
    )

    premature_claim = copy.deepcopy(valid)
    premature_claim["leadership_eligible"] = True
    require_invalid(
        lambda: validate_canonical_normalized_browser_report(premature_claim),
        "browser collector claimed leadership",
    )

    print("M7 normalized canonical browser full-set authority tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
