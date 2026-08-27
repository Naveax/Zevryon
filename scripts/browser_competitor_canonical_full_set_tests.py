#!/usr/bin/env python3
from __future__ import annotations

import copy

from browser_competitor_benchmark_evidence import HARNESS_SCHEMA, synthetic_corpus_sha256
from browser_competitor_canonical_full_set import (
    CanonicalFullSetInvalid,
    validate_canonical_full_set_report,
    validate_zevryon_corpus_report,
)
from browser_competitor_registry import CANONICAL_KEYS


PAYLOAD_BYTES = 64 * 1024 * 1024


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


def coverage() -> dict[str, object]:
    return {
        "canonical_requested": list(CANONICAL_KEYS),
        "canonical_measured": list(CANONICAL_KEYS),
        "canonical_missing": [],
        "canonical_unsuccessful": [],
        "comparison_mismatches": {},
        "full_set_coverage": True,
        "comparable_full_set": True,
        "leadership_evidence_gate_passed": True,
        "leadership_metric_gate_evaluated": False,
        "leadership_eligible": False,
    }


def report() -> dict[str, object]:
    return {
        "schema": HARNESS_SCHEMA,
        "requested_competitors": list(CANONICAL_KEYS),
        "all_requested_cases_succeeded": True,
        "leadership_coverage_by_mode": {
            "virtualized": coverage(),
            "native-dom": coverage(),
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
        lambda: validate_zevryon_corpus_report(drifted_authority, PAYLOAD_BYTES),
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

    native_failure = copy.deepcopy(valid)
    native_failure["all_requested_cases_succeeded"] = False
    native_failure["leadership_coverage_by_mode"]["native-dom"][
        "canonical_unsuccessful"
    ] = ["ladybird"]
    native_failure["leadership_coverage_by_mode"]["native-dom"][
        "full_set_coverage"
    ] = False
    native_failure["leadership_coverage_by_mode"]["native-dom"][
        "comparable_full_set"
    ] = False
    native_failure["leadership_coverage_by_mode"]["native-dom"][
        "leadership_evidence_gate_passed"
    ] = False
    require_invalid(
        lambda: validate_canonical_full_set_report(native_failure),
        "native-DOM failure was accepted",
    )

    identity_drift = copy.deepcopy(valid)
    identity_drift["leadership_coverage_by_mode"]["virtualized"][
        "comparison_mismatches"
    ] = {"system_fingerprint": ["edge"]}
    identity_drift["leadership_coverage_by_mode"]["virtualized"][
        "comparable_full_set"
    ] = False
    identity_drift["leadership_coverage_by_mode"]["virtualized"][
        "leadership_evidence_gate_passed"
    ] = False
    require_invalid(
        lambda: validate_canonical_full_set_report(identity_drift),
        "comparison identity drift was accepted",
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

    print("canonical full-set evidence and Zevryon corpus parity gate tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
