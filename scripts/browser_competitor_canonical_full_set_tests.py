#!/usr/bin/env python3
from __future__ import annotations

import copy

from browser_competitor_canonical_full_set import (
    CanonicalFullSetInvalid,
    validate_canonical_full_set_report,
)
from browser_competitor_benchmark_evidence import HARNESS_SCHEMA
from browser_competitor_registry import CANONICAL_KEYS


def require_invalid(callable_, message: str) -> None:
    try:
        callable_()
    except CanonicalFullSetInvalid:
        return
    raise AssertionError(message)


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

    print("canonical full-set evidence gate tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
