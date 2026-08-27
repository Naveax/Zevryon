#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys
from typing import Any, Mapping

from browser_competitor_benchmark import run_case
from browser_competitor_benchmark_evidence import (
    HARNESS_SCHEMA,
    evidence_identity,
    host_metadata,
    scenario_fingerprint,
    synthetic_corpus_sha256,
)
from browser_competitor_benchmark_plan import (
    BENCHMARK_MODES,
    plan_benchmark_cases,
)
from browser_competitor_normalized_core_evidence import (
    IDENTITY_KEYS,
    NormalizedCoreEvidenceInvalid,
    assert_comparable_core_evidence,
    validate_normalized_core_evidence,
)
from browser_competitor_query_plan import DEFAULT_WARMUP_QUERY_COUNT
from browser_competitor_registry import (
    CANONICAL_KEYS,
    leadership_coverage,
    validate_terminal_record,
)


COLLECTION_AUTHORITY = "m7-normalized-canonical-browser-full-set-v1"
CANONICAL_MODES = tuple(BENCHMARK_MODES)


class CanonicalNormalizedBrowserSetInvalid(ValueError):
    pass


def _positive_int(value: object, field: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
        raise CanonicalNormalizedBrowserSetInvalid(
            f"{field} must be a positive integer"
        )
    return value


def _nonnegative_int(value: object, field: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise CanonicalNormalizedBrowserSetInvalid(
            f"{field} must be a non-negative integer"
        )
    return value


def _timeout_for_mode(
    mode: str,
    *,
    virtual_timeout_seconds: int,
    native_timeout_seconds: int,
) -> int:
    if mode == "virtualized":
        return virtual_timeout_seconds
    if mode == "native-dom":
        return native_timeout_seconds
    raise CanonicalNormalizedBrowserSetInvalid(f"unknown benchmark mode: {mode}")


def _coverage_by_mode(cases: list[dict[str, Any]]) -> dict[str, object]:
    output: dict[str, object] = {}
    for mode in CANONICAL_MODES:
        output[mode] = leadership_coverage(
            [
                case
                for case in cases
                if case.get("mode") == mode and case.get("canonical") is True
            ]
        )
    return output


def _status_sets(cases: list[dict[str, Any]]) -> dict[str, list[str]]:
    by_competitor = {
        competitor: [
            case for case in cases if case.get("competitor") == competitor
        ]
        for competitor in CANONICAL_KEYS
    }
    return {
        "available_competitors": [
            competitor
            for competitor, records in by_competitor.items()
            if records and any(record.get("status") != "unavailable" for record in records)
        ],
        "unavailable_competitors": [
            competitor
            for competitor, records in by_competitor.items()
            if records and all(record.get("status") == "unavailable" for record in records)
        ],
        "successfully_measured_competitors": [
            competitor
            for competitor, records in by_competitor.items()
            if any(record.get("status") == "success" for record in records)
        ],
        "fully_measured_competitors": [
            competitor
            for competitor, records in by_competitor.items()
            if records and all(record.get("status") == "success" for record in records)
        ],
    }


def collect_canonical_normalized_browser_report(
    *,
    payload_bytes: int,
    query_count: int,
    warmup_query_count: int,
    virtual_slice_bytes: int,
    virtual_timeout_seconds: int,
    native_timeout_seconds: int,
) -> dict[str, object]:
    _positive_int(payload_bytes, "payload_bytes")
    _positive_int(query_count, "query_count")
    _nonnegative_int(warmup_query_count, "warmup_query_count")
    _positive_int(virtual_slice_bytes, "virtual_slice_bytes")
    _positive_int(virtual_timeout_seconds, "virtual_timeout_seconds")
    _positive_int(native_timeout_seconds, "native_timeout_seconds")

    plans = plan_benchmark_cases(CANONICAL_KEYS, CANONICAL_MODES)
    expected_plan_count = len(CANONICAL_KEYS) * len(CANONICAL_MODES)
    if len(plans) != expected_plan_count:
        raise CanonicalNormalizedBrowserSetInvalid(
            "canonical benchmark planner did not produce the exact case matrix"
        )

    host = host_metadata()
    corpus_sha256 = synthetic_corpus_sha256(payload_bytes)
    cases: list[dict[str, Any]] = []
    for plan in plans:
        timeout_seconds = _timeout_for_mode(
            plan.mode,
            virtual_timeout_seconds=virtual_timeout_seconds,
            native_timeout_seconds=native_timeout_seconds,
        )
        scenario_sha256 = scenario_fingerprint(
            mode=plan.mode,
            payload_bytes=payload_bytes,
            query_count=query_count,
            warmup_query_count=warmup_query_count,
            virtual_slice_bytes=virtual_slice_bytes,
            timeout_seconds=timeout_seconds,
        )
        identity = evidence_identity(
            host=host,
            corpus_sha256=corpus_sha256,
            scenario_sha256=scenario_sha256,
        )
        cases.append(
            run_case(
                plan,
                payload_bytes,
                query_count,
                warmup_query_count,
                virtual_slice_bytes,
                timeout_seconds,
                identity.as_terminal_kwargs(),
            )
        )

    failures = [case for case in cases if case.get("status") != "success"]
    report: dict[str, object] = {
        "schema": HARNESS_SCHEMA,
        "collection_authority": COLLECTION_AUTHORITY,
        "host": host,
        "corpus_sha256": corpus_sha256,
        "payload_bytes": payload_bytes,
        "query_count": query_count,
        "warmup_query_count": warmup_query_count,
        "virtual_slice_bytes": virtual_slice_bytes,
        "virtual_timeout_seconds": virtual_timeout_seconds,
        "native_timeout_seconds": native_timeout_seconds,
        "requested_competitors": list(CANONICAL_KEYS),
        **_status_sets(cases),
        "scope_notes": [
            "This normalized canonical browser collection is independent of the legacy Zevryon giant-document diagnostic report.",
            "Each canonical browser case uses the admitted case-owned runtime launch, post-warmup ready boundary, implementation-local query timing, and process-tree memory scope.",
            "Chrome and Edge require their exact branded Playwright channels; bundled Chromium is never substituted.",
            "Servo and Ladybird require their exact WebDriver runtime identities; unavailable runtimes remain explicit failed canonical evidence.",
        ],
        "browser_cases": cases,
        "leadership_coverage_by_mode": _coverage_by_mode(cases),
        "all_requested_cases_succeeded": not failures,
        "leadership_metric_gate_evaluated": False,
        "leadership_eligible": False,
    }
    return report


def _validated_cases(
    report: Mapping[str, object],
) -> dict[tuple[str, str], Mapping[str, object]]:
    payload_bytes = _positive_int(report.get("payload_bytes"), "payload_bytes")
    query_count = _positive_int(report.get("query_count"), "query_count")
    warmup_count = _nonnegative_int(
        report.get("warmup_query_count"), "warmup_query_count"
    )
    _positive_int(report.get("virtual_slice_bytes"), "virtual_slice_bytes")
    expected_corpus_sha = synthetic_corpus_sha256(payload_bytes)
    if report.get("corpus_sha256") != expected_corpus_sha:
        raise CanonicalNormalizedBrowserSetInvalid(
            "top-level corpus SHA differs from deterministic M7 synthetic authority"
        )

    raw_cases = report.get("browser_cases")
    expected_count = len(CANONICAL_KEYS) * len(CANONICAL_MODES)
    if not isinstance(raw_cases, list) or len(raw_cases) != expected_count:
        raise CanonicalNormalizedBrowserSetInvalid(
            f"report must contain exactly {expected_count} canonical browser cases"
        )

    cases: dict[tuple[str, str], Mapping[str, object]] = {}
    for index, raw_case in enumerate(raw_cases):
        if not isinstance(raw_case, Mapping):
            raise CanonicalNormalizedBrowserSetInvalid(
                f"browser case {index} is not an object"
            )
        try:
            spec = validate_terminal_record(raw_case)
        except ValueError as exc:
            raise CanonicalNormalizedBrowserSetInvalid(
                f"browser case {index} terminal evidence invalid: {exc}"
            ) from exc
        if not spec.canonical or spec.key not in CANONICAL_KEYS:
            raise CanonicalNormalizedBrowserSetInvalid(
                f"browser case {index} is not canonical"
            )
        mode = raw_case.get("mode")
        if mode not in CANONICAL_MODES:
            raise CanonicalNormalizedBrowserSetInvalid(
                f"browser case {index} has invalid benchmark mode"
            )
        key = (spec.key, str(mode))
        if key in cases:
            raise CanonicalNormalizedBrowserSetInvalid(
                f"duplicate canonical browser case: {spec.key}/{mode}"
            )
        if raw_case.get("status") != "success":
            raise CanonicalNormalizedBrowserSetInvalid(
                f"canonical browser case did not succeed: {spec.key}/{mode}"
            )
        if raw_case.get("browser") != spec.key:
            raise CanonicalNormalizedBrowserSetInvalid(
                f"canonical browser case identity drifted: {spec.key}/{mode}"
            )
        if raw_case.get("payload_bytes") != payload_bytes:
            raise CanonicalNormalizedBrowserSetInvalid(
                f"canonical browser payload size drifted: {spec.key}/{mode}"
            )
        if raw_case.get("corpus_sha256") != expected_corpus_sha:
            raise CanonicalNormalizedBrowserSetInvalid(
                f"canonical browser terminal corpus SHA drifted: {spec.key}/{mode}"
            )
        if raw_case.get("query_count") != query_count:
            raise CanonicalNormalizedBrowserSetInvalid(
                f"canonical browser query count drifted: {spec.key}/{mode}"
            )
        if raw_case.get("warmup_query_count") != warmup_count:
            raise CanonicalNormalizedBrowserSetInvalid(
                f"canonical browser warmup count drifted: {spec.key}/{mode}"
            )

        normalized = raw_case.get("normalized_core_evidence")
        if not isinstance(normalized, Mapping):
            raise CanonicalNormalizedBrowserSetInvalid(
                f"canonical browser case lacks normalized evidence: {spec.key}/{mode}"
            )
        try:
            validate_normalized_core_evidence(normalized)
        except NormalizedCoreEvidenceInvalid as exc:
            raise CanonicalNormalizedBrowserSetInvalid(
                f"canonical browser normalized evidence invalid: {spec.key}/{mode}: {exc}"
            ) from exc
        drift = [
            field
            for field in IDENTITY_KEYS
            if normalized.get(field) != raw_case.get(field)
        ]
        if drift:
            raise CanonicalNormalizedBrowserSetInvalid(
                f"canonical browser terminal/normalized identity drifted: {spec.key}/{mode}: "
                + ", ".join(drift)
            )
        if normalized.get("corpus_sha256") != expected_corpus_sha:
            raise CanonicalNormalizedBrowserSetInvalid(
                f"canonical browser normalized corpus SHA drifted: {spec.key}/{mode}"
            )
        if normalized.get("query_sample_count") != query_count:
            raise CanonicalNormalizedBrowserSetInvalid(
                f"canonical browser normalized query count drifted: {spec.key}/{mode}"
            )
        if normalized.get("warmup_query_count") != warmup_count:
            raise CanonicalNormalizedBrowserSetInvalid(
                f"canonical browser normalized warmup count drifted: {spec.key}/{mode}"
            )
        cases[key] = raw_case

    expected_keys = {
        (competitor, mode)
        for competitor in CANONICAL_KEYS
        for mode in CANONICAL_MODES
    }
    if set(cases) != expected_keys:
        raise CanonicalNormalizedBrowserSetInvalid(
            "canonical browser case matrix does not match the exact 6x2 authority"
        )
    return cases


def validate_canonical_normalized_browser_report(
    report: Mapping[str, object],
) -> None:
    if report.get("schema") != HARNESS_SCHEMA:
        raise CanonicalNormalizedBrowserSetInvalid("browser report schema mismatch")
    if report.get("collection_authority") != COLLECTION_AUTHORITY:
        raise CanonicalNormalizedBrowserSetInvalid(
            "browser report collection authority mismatch"
        )
    if report.get("requested_competitors") != list(CANONICAL_KEYS):
        raise CanonicalNormalizedBrowserSetInvalid(
            "browser report did not request the exact canonical competitor set"
        )
    if report.get("all_requested_cases_succeeded") is not True:
        raise CanonicalNormalizedBrowserSetInvalid(
            "one or more canonical browser cases failed"
        )

    cases = _validated_cases(report)
    coverage_by_mode = report.get("leadership_coverage_by_mode")
    if not isinstance(coverage_by_mode, Mapping):
        raise CanonicalNormalizedBrowserSetInvalid(
            "browser report lacks per-mode leadership coverage"
        )

    for mode in CANONICAL_MODES:
        mode_cases = [cases[(competitor, mode)] for competitor in CANONICAL_KEYS]
        normalized_records = [
            case["normalized_core_evidence"] for case in mode_cases
        ]
        try:
            assert_comparable_core_evidence(normalized_records)
        except NormalizedCoreEvidenceInvalid as exc:
            raise CanonicalNormalizedBrowserSetInvalid(
                f"{mode} normalized browser evidence is not comparable: {exc}"
            ) from exc

        coverage = coverage_by_mode.get(mode)
        if not isinstance(coverage, Mapping):
            raise CanonicalNormalizedBrowserSetInvalid(
                f"browser report lacks {mode} coverage"
            )
        try:
            expected_coverage = leadership_coverage(mode_cases)
        except ValueError as exc:
            raise CanonicalNormalizedBrowserSetInvalid(
                f"{mode} coverage recomputation failed: {exc}"
            ) from exc
        if dict(coverage) != expected_coverage:
            raise CanonicalNormalizedBrowserSetInvalid(
                f"{mode} leadership coverage does not match raw case evidence"
            )
        if coverage.get("leadership_evidence_gate_passed") is not True:
            raise CanonicalNormalizedBrowserSetInvalid(
                f"{mode} leadership evidence gate did not pass"
            )
        if coverage.get("leadership_metric_gate_evaluated") is not False:
            raise CanonicalNormalizedBrowserSetInvalid(
                f"{mode} coverage unexpectedly evaluated the metric gate"
            )
        if coverage.get("leadership_eligible") is not False:
            raise CanonicalNormalizedBrowserSetInvalid(
                f"{mode} coverage claimed leadership before metric evaluation"
            )

    if report.get("leadership_metric_gate_evaluated") is not False:
        raise CanonicalNormalizedBrowserSetInvalid(
            "browser collection cannot evaluate the leadership metric gate"
        )
    if report.get("leadership_eligible") is not False:
        raise CanonicalNormalizedBrowserSetInvalid(
            "browser collection cannot claim leadership"
        )


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Collect the exact canonical six-browser M7 normalized full set "
            "without depending on the legacy Zevryon diagnostic benchmark."
        )
    )
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--payload-bytes", type=int, default=64 * 1024 * 1024)
    parser.add_argument("--query-count", type=int, default=21)
    parser.add_argument(
        "--warmup-query-count",
        type=int,
        default=DEFAULT_WARMUP_QUERY_COUNT,
    )
    parser.add_argument("--virtual-slice-bytes", type=int, default=128 * 1024)
    parser.add_argument("--virtual-timeout-seconds", type=int, default=180)
    parser.add_argument("--native-timeout-seconds", type=int, default=420)
    args = parser.parse_args()

    try:
        report = collect_canonical_normalized_browser_report(
            payload_bytes=args.payload_bytes,
            query_count=args.query_count,
            warmup_query_count=args.warmup_query_count,
            virtual_slice_bytes=args.virtual_slice_bytes,
            virtual_timeout_seconds=args.virtual_timeout_seconds,
            native_timeout_seconds=args.native_timeout_seconds,
        )
    except (ValueError, CanonicalNormalizedBrowserSetInvalid) as exc:
        print(f"canonical normalized browser collection failed: {exc}", file=sys.stderr)
        return 1

    text = json.dumps(report, indent=2, sort_keys=True) + "\n"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(text, encoding="utf-8")
    print(text, end="")

    try:
        validate_canonical_normalized_browser_report(report)
    except CanonicalNormalizedBrowserSetInvalid as exc:
        print(f"canonical normalized browser evidence rejected: {exc}", file=sys.stderr)
        return 1
    print("canonical normalized browser full-set gate: passed", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
