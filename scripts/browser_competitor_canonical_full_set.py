#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys
from typing import Mapping

from browser_competitor_benchmark_evidence import (
    HARNESS_SCHEMA,
    synthetic_corpus_sha256,
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


ZEVRYON_BENCHMARK_SCHEMA = "zevryon.massivedoc.benchmark.v4"
_CANONICAL_MODES = ("virtualized", "native-dom")


class CanonicalFullSetInvalid(ValueError):
    pass


def validate_zevryon_corpus_report(
    report: Mapping[str, object], payload_bytes: int
) -> None:
    if payload_bytes <= 0:
        raise CanonicalFullSetInvalid("canonical payload bytes must be positive")
    if report.get("schema") != ZEVRYON_BENCHMARK_SCHEMA:
        raise CanonicalFullSetInvalid("Zevryon benchmark report schema mismatch")
    if report.get("giant_record_bytes") != payload_bytes:
        raise CanonicalFullSetInvalid("Zevryon giant record size differs from browser payload")
    if report.get("giant_record_profile") != "m7-competitor":
        raise CanonicalFullSetInvalid(
            "Zevryon giant record does not use the canonical M7 profile"
        )
    giant_index = report.get("giant_record_index")
    if isinstance(giant_index, bool) or not isinstance(giant_index, int) or giant_index < 0:
        raise CanonicalFullSetInvalid("Zevryon giant record index is invalid")

    expected_sha = synthetic_corpus_sha256(payload_bytes)
    if report.get("giant_record_sha256") != expected_sha:
        raise CanonicalFullSetInvalid("Zevryon giant record SHA differs from browser payload")
    if report.get("giant_record_expected_m7_sha256") != expected_sha:
        raise CanonicalFullSetInvalid("Zevryon report expected-M7 SHA authority drifted")
    if report.get("giant_record_matches_m7_synthetic") is not True:
        raise CanonicalFullSetInvalid(
            "Zevryon report did not prove exact M7 corpus parity"
        )


def _positive_int(value: object, field: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
        raise CanonicalFullSetInvalid(f"{field} must be a positive integer")
    return value


def _nonnegative_int(value: object, field: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise CanonicalFullSetInvalid(f"{field} must be a non-negative integer")
    return value


def _validated_cases(
    report: Mapping[str, object],
) -> dict[tuple[str, str], Mapping[str, object]]:
    raw_cases = report.get("browser_cases")
    expected_count = len(CANONICAL_KEYS) * len(_CANONICAL_MODES)
    if not isinstance(raw_cases, list) or len(raw_cases) != expected_count:
        raise CanonicalFullSetInvalid(
            f"benchmark report must contain exactly {expected_count} canonical browser cases"
        )

    query_count = _positive_int(report.get("query_count"), "query_count")
    warmup_count = _nonnegative_int(
        report.get("warmup_query_count"), "warmup_query_count"
    )

    cases: dict[tuple[str, str], Mapping[str, object]] = {}
    for index, raw_case in enumerate(raw_cases):
        if not isinstance(raw_case, Mapping):
            raise CanonicalFullSetInvalid(f"browser case {index} is not an object")
        try:
            spec = validate_terminal_record(raw_case)
        except ValueError as exc:
            raise CanonicalFullSetInvalid(
                f"browser case {index} terminal evidence invalid: {exc}"
            ) from exc
        if not spec.canonical or spec.key not in CANONICAL_KEYS:
            raise CanonicalFullSetInvalid(
                f"browser case {index} is not a canonical competitor"
            )
        mode = raw_case.get("mode")
        if mode not in _CANONICAL_MODES:
            raise CanonicalFullSetInvalid(
                f"browser case {index} has invalid benchmark mode"
            )
        key = (spec.key, str(mode))
        if key in cases:
            raise CanonicalFullSetInvalid(
                f"duplicate canonical browser case: {spec.key}/{mode}"
            )
        if raw_case.get("status") != "success":
            raise CanonicalFullSetInvalid(
                f"canonical browser case did not succeed: {spec.key}/{mode}"
            )
        if raw_case.get("browser") != spec.key:
            raise CanonicalFullSetInvalid(
                f"canonical browser case identity drifted: {spec.key}/{mode}"
            )
        if raw_case.get("query_count") != query_count:
            raise CanonicalFullSetInvalid(
                f"canonical browser query count drifted: {spec.key}/{mode}"
            )
        if raw_case.get("warmup_query_count") != warmup_count:
            raise CanonicalFullSetInvalid(
                f"canonical browser warmup count drifted: {spec.key}/{mode}"
            )

        normalized = raw_case.get("normalized_core_evidence")
        if not isinstance(normalized, Mapping):
            raise CanonicalFullSetInvalid(
                f"canonical browser case lacks normalized core evidence: {spec.key}/{mode}"
            )
        try:
            validate_normalized_core_evidence(normalized)
        except NormalizedCoreEvidenceInvalid as exc:
            raise CanonicalFullSetInvalid(
                f"canonical browser normalized evidence invalid: {spec.key}/{mode}: {exc}"
            ) from exc
        drift = [
            field for field in IDENTITY_KEYS
            if normalized.get(field) != raw_case.get(field)
        ]
        if drift:
            raise CanonicalFullSetInvalid(
                f"canonical browser normalized identity drifted: {spec.key}/{mode}: "
                + ", ".join(drift)
            )
        if normalized.get("query_sample_count") != query_count:
            raise CanonicalFullSetInvalid(
                f"canonical browser normalized query count drifted: {spec.key}/{mode}"
            )
        if normalized.get("warmup_query_count") != warmup_count:
            raise CanonicalFullSetInvalid(
                f"canonical browser normalized warmup count drifted: {spec.key}/{mode}"
            )
        cases[key] = raw_case

    expected_keys = {
        (competitor, mode)
        for competitor in CANONICAL_KEYS
        for mode in _CANONICAL_MODES
    }
    if set(cases) != expected_keys:
        missing = sorted(expected_keys - set(cases))
        extra = sorted(set(cases) - expected_keys)
        raise CanonicalFullSetInvalid(
            f"canonical browser case matrix drifted; missing={missing}, extra={extra}"
        )
    return cases


def validate_canonical_full_set_report(report: Mapping[str, object]) -> None:
    if report.get("schema") != HARNESS_SCHEMA:
        raise CanonicalFullSetInvalid("benchmark report schema mismatch")

    requested = report.get("requested_competitors")
    if requested != list(CANONICAL_KEYS):
        raise CanonicalFullSetInvalid(
            "benchmark report did not request the exact canonical competitor set"
        )
    if report.get("all_requested_cases_succeeded") is not True:
        raise CanonicalFullSetInvalid("one or more canonical benchmark cases failed")

    cases = _validated_cases(report)
    coverage_by_mode = report.get("leadership_coverage_by_mode")
    if not isinstance(coverage_by_mode, Mapping):
        raise CanonicalFullSetInvalid(
            "benchmark report lacks per-mode leadership coverage"
        )

    for mode in _CANONICAL_MODES:
        mode_cases = [cases[(competitor, mode)] for competitor in CANONICAL_KEYS]
        normalized_records = [
            case["normalized_core_evidence"] for case in mode_cases
        ]
        try:
            assert_comparable_core_evidence(normalized_records)
        except NormalizedCoreEvidenceInvalid as exc:
            raise CanonicalFullSetInvalid(
                f"{mode} normalized evidence is not comparable: {exc}"
            ) from exc

        coverage = coverage_by_mode.get(mode)
        if not isinstance(coverage, Mapping):
            raise CanonicalFullSetInvalid(f"benchmark report lacks {mode} coverage")
        try:
            expected_coverage = leadership_coverage(mode_cases)
        except ValueError as exc:
            raise CanonicalFullSetInvalid(
                f"{mode} coverage recomputation failed: {exc}"
            ) from exc
        if dict(coverage) != expected_coverage:
            raise CanonicalFullSetInvalid(
                f"{mode} leadership coverage does not match canonical case evidence"
            )
        if coverage.get("leadership_evidence_gate_passed") is not True:
            raise CanonicalFullSetInvalid(
                f"{mode} leadership evidence gate did not pass"
            )
        if coverage.get("leadership_metric_gate_evaluated") is not False:
            raise CanonicalFullSetInvalid(
                f"{mode} report unexpectedly evaluated the metric gate"
            )
        if coverage.get("leadership_eligible") is not False:
            raise CanonicalFullSetInvalid(
                f"{mode} report claimed leadership before metric evaluation"
            )

    if report.get("leadership_metric_gate_evaluated") is not False:
        raise CanonicalFullSetInvalid(
            "canonical full-set evidence cannot evaluate the metric gate"
        )
    if report.get("leadership_eligible") is not False:
        raise CanonicalFullSetInvalid(
            "canonical full-set evidence cannot claim leadership"
        )


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Run the exact canonical M7 competitor set and fail closed unless both "
            "benchmark modes produce complete comparable normalized evidence"
        )
    )
    parser.add_argument("--zevryon-report", type=Path, required=True)
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

    if (
        args.payload_bytes <= 0
        or args.query_count <= 0
        or args.warmup_query_count < 0
        or args.virtual_slice_bytes <= 0
        or args.virtual_timeout_seconds <= 0
        or args.native_timeout_seconds <= 0
    ):
        parser.error(
            "sizes/counts/timeouts must be positive and warmup count non-negative"
        )

    try:
        zevryon_report = json.loads(
            args.zevryon_report.read_text(encoding="utf-8")
        )
        if not isinstance(zevryon_report, dict):
            raise CanonicalFullSetInvalid(
                "Zevryon benchmark report is not a JSON object"
            )
        validate_zevryon_corpus_report(zevryon_report, args.payload_bytes)
    except (OSError, json.JSONDecodeError, CanonicalFullSetInvalid) as exc:
        print(
            f"canonical full-set Zevryon corpus rejected: {exc}",
            file=sys.stderr,
        )
        return 1

    benchmark = Path(__file__).with_name("browser_competitor_benchmark.py")
    command = [
        sys.executable,
        str(benchmark),
        "--zevryon-report",
        str(args.zevryon_report),
        "--output",
        str(args.output),
        "--payload-bytes",
        str(args.payload_bytes),
        "--query-count",
        str(args.query_count),
        "--warmup-query-count",
        str(args.warmup_query_count),
        "--virtual-slice-bytes",
        str(args.virtual_slice_bytes),
        "--virtual-timeout-seconds",
        str(args.virtual_timeout_seconds),
        "--native-timeout-seconds",
        str(args.native_timeout_seconds),
    ]
    for competitor in CANONICAL_KEYS:
        command.extend(["--competitor", competitor])

    completed = subprocess.run(command, check=False)
    if not args.output.is_file():
        return completed.returncode if completed.returncode != 0 else 1

    try:
        report = json.loads(args.output.read_text(encoding="utf-8"))
        if not isinstance(report, dict):
            raise CanonicalFullSetInvalid(
                "benchmark report is not a JSON object"
            )
        validate_canonical_full_set_report(report)
    except (OSError, json.JSONDecodeError, CanonicalFullSetInvalid) as exc:
        print(
            f"canonical full-set evidence rejected: {exc}",
            file=sys.stderr,
        )
        return 1

    if completed.returncode != 0:
        print(
            "canonical full-set benchmark returned non-zero despite a passing report",
            file=sys.stderr,
        )
        return 1
    print("canonical full-set normalized evidence gate: passed", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
