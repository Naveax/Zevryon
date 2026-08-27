#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import sys
from typing import Mapping

from browser_competitor_canonical_full_set import (
    CanonicalFullSetInvalid,
    validate_canonical_full_set_report,
)
from browser_competitor_normalized_core_evidence import (
    CORE_METRIC_KEYS,
    IDENTITY_KEYS,
    NormalizedCoreEvidenceInvalid,
    assert_comparable_core_evidence,
    validate_normalized_core_evidence,
)
from browser_competitor_registry import CANONICAL_KEYS
from m7_zevryon_normalized_case import (
    CASE_SCHEMA,
    M7_SOURCE_AUTHORITY,
    ZevryonNormalizedCaseInvalid,
    validate_session_events,
)


EVALUATOR_SCHEMA = "zevryon.competitor.leadership-evaluation.v1"
CANONICAL_MODES = ("virtualized", "native-dom")
FIRST_REQUIRED = 4
WITHIN_FRACTION = 0.05


class LeadershipEvaluationInvalid(ValueError):
    pass


def _mapping(value: object, field: str) -> Mapping[str, object]:
    if not isinstance(value, Mapping):
        raise LeadershipEvaluationInvalid(f"{field} must be an object")
    return value


def _positive_int(value: object, field: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
        raise LeadershipEvaluationInvalid(f"{field} must be a positive integer")
    return value


def _nonnegative_int(value: object, field: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise LeadershipEvaluationInvalid(f"{field} must be a non-negative integer")
    return value


def _finite_nonnegative(value: object, field: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise LeadershipEvaluationInvalid(f"{field} must be numeric")
    normalized = float(value)
    if not math.isfinite(normalized) or normalized < 0.0:
        raise LeadershipEvaluationInvalid(
            f"{field} must be finite and non-negative"
        )
    return normalized


def _close(left: object, right: object) -> bool:
    try:
        return math.isclose(float(left), float(right), rel_tol=1e-12, abs_tol=1e-9)
    except (TypeError, ValueError, OverflowError):
        return False


def validate_zevryon_normalized_case(
    record: Mapping[str, object],
    *,
    expected_mode: str,
) -> Mapping[str, object]:
    if expected_mode not in CANONICAL_MODES:
        raise LeadershipEvaluationInvalid(f"unknown canonical mode: {expected_mode}")
    if record.get("schema") != CASE_SCHEMA:
        raise LeadershipEvaluationInvalid("Zevryon normalized case schema mismatch")
    if record.get("implementation") != "zevryon":
        raise LeadershipEvaluationInvalid("normalized case is not Zevryon")
    if record.get("status") != "success":
        raise LeadershipEvaluationInvalid("Zevryon normalized case did not succeed")
    if record.get("mode") != expected_mode:
        raise LeadershipEvaluationInvalid("Zevryon normalized case mode drifted")
    if record.get("source_authority") != M7_SOURCE_AUTHORITY:
        raise LeadershipEvaluationInvalid(
            "Zevryon normalized case does not use case-owned M7 source authority"
        )
    record_index = _nonnegative_int(record.get("record_index"), "record_index")
    if record_index != 0:
        raise LeadershipEvaluationInvalid(
            "Zevryon normalized case must use the single canonical record"
        )

    payload_bytes = _positive_int(record.get("payload_bytes"), "payload_bytes")
    query_count = _positive_int(record.get("query_count"), "query_count")
    warmup_count = _nonnegative_int(
        record.get("warmup_query_count"), "warmup_query_count"
    )
    _positive_int(record.get("timeout_seconds"), "timeout_seconds")
    runtime_identity = record.get("runtime_identity")
    if not isinstance(runtime_identity, str) or not runtime_identity.strip():
        raise LeadershipEvaluationInvalid("Zevryon runtime identity is missing")

    ready = _mapping(record.get("session_ready"), "session_ready")
    details = record.get("query_details")
    if not isinstance(details, list):
        raise LeadershipEvaluationInvalid("query_details must be an array")
    complete = _mapping(record.get("session_complete"), "session_complete")
    slice_bytes = _positive_int(
        ready.get("virtual_slice_bytes"), "session_ready.virtual_slice_bytes"
    )
    if expected_mode == "virtualized":
        if record.get("virtual_slice_bytes") != slice_bytes:
            raise LeadershipEvaluationInvalid(
                "Zevryon virtualized slice authority drifted"
            )
    elif record.get("virtual_slice_bytes") is not None:
        raise LeadershipEvaluationInvalid(
            "native-DOM Zevryon case must not publish a virtualized metric parameter"
        )

    events: list[Mapping[str, object]] = [ready]
    for index, detail in enumerate(details):
        if not isinstance(detail, Mapping):
            raise LeadershipEvaluationInvalid(
                f"Zevryon query detail {index} is not an object"
            )
        events.append(detail)
    events.append(complete)
    try:
        transcript = validate_session_events(
            events,
            mode=expected_mode,
            payload_bytes=payload_bytes,
            query_count=query_count,
            warmup_query_count=warmup_count,
            virtual_slice_bytes=slice_bytes,
        )
    except ZevryonNormalizedCaseInvalid as exc:
        raise LeadershipEvaluationInvalid(
            f"Zevryon persistent-session transcript invalid: {exc}"
        ) from exc

    normalized = _mapping(
        record.get("normalized_core_evidence"),
        "normalized_core_evidence",
    )
    try:
        validate_normalized_core_evidence(normalized)
    except NormalizedCoreEvidenceInvalid as exc:
        raise LeadershipEvaluationInvalid(
            f"Zevryon normalized evidence invalid: {exc}"
        ) from exc

    identity_drift = [
        key for key in IDENTITY_KEYS if record.get(key) != normalized.get(key)
    ]
    if identity_drift:
        raise LeadershipEvaluationInvalid(
            "Zevryon terminal/normalized identity drifted: "
            + ", ".join(identity_drift)
        )
    if normalized.get("query_sample_count") != query_count:
        raise LeadershipEvaluationInvalid(
            "Zevryon normalized query count drifted from case authority"
        )
    if normalized.get("warmup_query_count") != warmup_count:
        raise LeadershipEvaluationInvalid(
            "Zevryon normalized warmup count drifted from case authority"
        )

    samples = normalized.get("query_samples_ms")
    if not isinstance(samples, list) or len(samples) != query_count:
        raise LeadershipEvaluationInvalid(
            "Zevryon normalized raw sample cardinality drifted"
        )
    transcript_samples = transcript.query_samples_ms
    for index, (sample, receipt_sample) in enumerate(zip(samples, transcript_samples)):
        if not _close(sample, receipt_sample):
            raise LeadershipEvaluationInvalid(
                f"Zevryon normalized query sample {index} drifted from transcript"
            )

    metrics = _mapping(normalized.get("core_metrics"), "core_metrics")
    if not _close(
        record.get("normalized_setup_to_ready_seconds"),
        metrics.get("setup_to_ready_seconds"),
    ):
        raise LeadershipEvaluationInvalid(
            "Zevryon setup metric drifted from normalized evidence"
        )
    if not _close(
        record.get("process_scope_peak_mb"),
        metrics.get("incremental_peak_memory_mb"),
    ):
        raise LeadershipEvaluationInvalid(
            "Zevryon memory metric drifted from normalized evidence"
        )
    return normalized


def _browser_mode_evidence(
    report: Mapping[str, object],
    mode: str,
) -> dict[str, Mapping[str, object]]:
    try:
        validate_canonical_full_set_report(report)
    except CanonicalFullSetInvalid as exc:
        raise LeadershipEvaluationInvalid(
            f"canonical browser report invalid: {exc}"
        ) from exc

    raw_cases = report.get("browser_cases")
    if not isinstance(raw_cases, list):
        raise LeadershipEvaluationInvalid("browser report lacks browser_cases")

    discovered: dict[str, Mapping[str, object]] = {}
    for case in raw_cases:
        if not isinstance(case, Mapping) or case.get("mode") != mode:
            continue
        competitor = case.get("competitor")
        if competitor not in CANONICAL_KEYS:
            raise LeadershipEvaluationInvalid(
                f"unexpected browser competitor in {mode} evidence"
            )
        normalized = _mapping(
            case.get("normalized_core_evidence"),
            f"{competitor}/{mode}.normalized_core_evidence",
        )
        discovered[str(competitor)] = normalized

    expected = set(CANONICAL_KEYS)
    actual = set(discovered)
    if actual != expected:
        raise LeadershipEvaluationInvalid(
            f"{mode} canonical browser evidence set drifted; "
            f"missing={sorted(expected - actual)}, extra={sorted(actual - expected)}"
        )
    return {key: discovered[key] for key in CANONICAL_KEYS}


def evaluate_mode(
    browser_report: Mapping[str, object],
    zevryon_case: Mapping[str, object],
    *,
    mode: str,
) -> dict[str, object]:
    zevryon = validate_zevryon_normalized_case(
        zevryon_case,
        expected_mode=mode,
    )
    browsers = _browser_mode_evidence(browser_report, mode)
    records = [zevryon, *[browsers[key] for key in CANONICAL_KEYS]]
    try:
        assert_comparable_core_evidence(records)
    except NormalizedCoreEvidenceInvalid as exc:
        raise LeadershipEvaluationInvalid(
            f"{mode} Zevryon/browser normalized evidence is not comparable: {exc}"
        ) from exc

    evidence_by_name: dict[str, Mapping[str, object]] = {
        "zevryon": zevryon,
        **browsers,
    }
    metric_results: dict[str, object] = {}
    first_count = 0
    every_nonfirst_within = True

    for metric in CORE_METRIC_KEYS:
        values: dict[str, float] = {}
        for name, evidence in evidence_by_name.items():
            metrics = _mapping(evidence.get("core_metrics"), f"{name}.core_metrics")
            values[name] = _finite_nonnegative(metrics.get(metric), f"{name}.{metric}")

        leader_value = min(values.values())
        if leader_value <= 0.0:
            raise LeadershipEvaluationInvalid(
                f"{mode}/{metric} leader value must be positive for this benchmark"
            )
        leaders = [name for name, value in values.items() if value == leader_value]
        zevryon_value = values["zevryon"]
        if zevryon_value == leader_value:
            status = "first"
            first_count += 1
        elif zevryon_value <= leader_value * (1.0 + WITHIN_FRACTION):
            status = "within_5_percent"
        else:
            status = "outside_5_percent"
            every_nonfirst_within = False

        ratio = zevryon_value / leader_value
        metric_results[metric] = {
            "leader_value": leader_value,
            "leaders": leaders,
            "zevryon_value": zevryon_value,
            "zevryon_ratio_to_leader": ratio,
            "zevryon_percent_above_leader": (ratio - 1.0) * 100.0,
            "zevryon_status": status,
            "values": values,
        }

    eligible = first_count >= FIRST_REQUIRED and every_nonfirst_within
    return {
        "mode": mode,
        "implementation_count": 1 + len(CANONICAL_KEYS),
        "canonical_browser_competitors": list(CANONICAL_KEYS),
        "first_required": FIRST_REQUIRED,
        "zevryon_first_count": first_count,
        "all_nonfirst_metrics_within_5_percent": every_nonfirst_within,
        "leadership_metric_gate_evaluated": True,
        "leadership_eligible": eligible,
        "metrics": metric_results,
    }


def evaluate_leadership(
    browser_report: Mapping[str, object],
    zevryon_virtualized: Mapping[str, object],
    zevryon_native_dom: Mapping[str, object],
) -> dict[str, object]:
    mode_results = {
        "virtualized": evaluate_mode(
            browser_report,
            zevryon_virtualized,
            mode="virtualized",
        ),
        "native-dom": evaluate_mode(
            browser_report,
            zevryon_native_dom,
            mode="native-dom",
        ),
    }
    overall = all(
        result.get("leadership_eligible") is True
        for result in mode_results.values()
    )
    return {
        "schema": EVALUATOR_SCHEMA,
        "core_metric_keys": list(CORE_METRIC_KEYS),
        "ranking_rule": {
            "lower_is_better": True,
            "first_required": FIRST_REQUIRED,
            "remaining_metric_max_percent_above_leader": WITHIN_FRACTION * 100.0,
            "ties_count_as_first": True,
            "all_canonical_modes_must_pass": True,
        },
        "modes": mode_results,
        "leadership_metric_gate_evaluated": True,
        "leadership_eligible": overall,
    }


def _read_object(path: Path, label: str) -> Mapping[str, object]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise LeadershipEvaluationInvalid(f"cannot read {label}: {exc}") from exc
    if not isinstance(value, Mapping):
        raise LeadershipEvaluationInvalid(f"{label} must be a JSON object")
    return value


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Evaluate the fixed five-metric M7 leadership rule from one canonical "
            "six-browser full-set report plus normalized Zevryon virtualized and "
            "native-DOM case evidence."
        )
    )
    parser.add_argument("--browser-report", type=Path, required=True)
    parser.add_argument("--zevryon-virtualized", type=Path, required=True)
    parser.add_argument("--zevryon-native-dom", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    try:
        result = evaluate_leadership(
            _read_object(args.browser_report, "browser report"),
            _read_object(args.zevryon_virtualized, "Zevryon virtualized evidence"),
            _read_object(args.zevryon_native_dom, "Zevryon native-DOM evidence"),
        )
    except LeadershipEvaluationInvalid as exc:
        print(f"M7 leadership evaluation rejected: {exc}", file=sys.stderr)
        return 1

    text = json.dumps(result, indent=2, sort_keys=True) + "\n"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(text, encoding="utf-8")
    print(text, end="")
    return 0 if result["leadership_eligible"] is True else 2


if __name__ == "__main__":
    raise SystemExit(main())
