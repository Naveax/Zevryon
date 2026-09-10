#!/usr/bin/env python3
from __future__ import annotations

import argparse
import copy
import json
import sys
from pathlib import Path
from typing import Any

from z7_html5lib_tokenizer_full_corpus_census_v1 import (
    CensusError,
    DEFAULT_MANIFEST,
    REPORT_SCHEMA,
    execute_census,
)

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BASELINE = ROOT / "config/z7_html5lib_tokenizer_full_corpus_census_v1.json"
BASELINE_SCHEMA = "zevryon.z7.html5lib-tokenizer-full-corpus-census-baseline.v1"
BASELINE_AUTHORITY = "z7-html5lib-tokenizer-full-corpus-census-v1"
PINNED_UPSTREAM_COMMIT = "224991ec10db04f056a89eed8b0bd8695fd2950e"
POLICIES = {"exact", "no-regression"}


class BaselineError(ValueError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise BaselineError(message)


def require_count(value: Any, label: str) -> int:
    require(isinstance(value, int) and not isinstance(value, bool) and value >= 0, f"{label} must be non-negative integer")
    return value


def load_baseline(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise BaselineError(f"cannot read census baseline: {exc}") from exc
    require(isinstance(value, dict), "census baseline root must be an object")
    require(value.get("schema") == BASELINE_SCHEMA, "census baseline schema mismatch")
    require(value.get("authority") == BASELINE_AUTHORITY, "census baseline authority mismatch")
    require(value.get("upstream_commit") == PINNED_UPSTREAM_COMMIT, "census baseline upstream commit drifted")
    require(value.get("files") == 14, "census baseline file denominator drifted")
    require(value.get("tests") == 6810, "census baseline test denominator drifted")
    require(value.get("executions") == 7036, "census baseline execution denominator drifted")
    require(value.get("passed") == 5660, "census baseline passed denominator drifted")
    require(value.get("failed") == 129, "census baseline failed denominator drifted")
    require(value.get("unsupported") == 1247, "census baseline unsupported denominator drifted")
    require(
        value["passed"] + value["failed"] + value["unsupported"] == value["executions"],
        "census baseline accounting drifted",
    )
    require(value.get("full_corpus_pass_claim") is False, "baseline must not claim full-corpus pass")
    require(value.get("html_tokenizer_conformance_claim") is False, "baseline must not claim tokenizer conformance")
    require(value.get("z7_status_change") is False, "baseline must not change Z7 status")
    require(isinstance(value.get("file_reports"), dict), "baseline file_reports must be object")
    require(isinstance(value.get("reason_counts"), dict), "baseline reason_counts must be object")
    return value


def verify_reason_accounting(report: dict[str, Any]) -> None:
    reasons = report.get("reason_counts")
    require(isinstance(reasons, dict), "census reason_counts must be object")
    for reason, count in reasons.items():
        require(isinstance(reason, str) and reason, "census reason key must be non-empty string")
        require_count(count, f"census reason count {reason!r}")
    passed_reasons = sum(count for reason, count in reasons.items() if reason.startswith("passed:"))
    failed_reasons = sum(count for reason, count in reasons.items() if reason.startswith("failed:"))
    unsupported_reasons = sum(count for reason, count in reasons.items() if reason.startswith("unsupported:"))
    require(passed_reasons == report["passed"], "census passed reason accounting drifted")
    require(failed_reasons == report["failed"], "census failed reason accounting drifted")
    require(unsupported_reasons == report["unsupported"], "census unsupported reason accounting drifted")
    require(passed_reasons + failed_reasons + unsupported_reasons == report["executions"], "census reason accounting denominator drifted")


def verify_file_accounting(report: dict[str, Any], baseline: dict[str, Any]) -> None:
    current_files = report.get("file_reports")
    baseline_files = baseline.get("file_reports")
    require(isinstance(current_files, dict), "census file_reports must be object")
    require(isinstance(baseline_files, dict), "baseline file_reports must be object")
    require(set(current_files) == set(baseline_files), "census fixture partition set drifted")
    for path, current in current_files.items():
        expected = baseline_files[path]
        require(isinstance(current, dict), f"census file report must be object: {path}")
        require(isinstance(expected, dict), f"baseline file report must be object: {path}")
        for key in ("executions", "passed", "failed", "unsupported"):
            require_count(current.get(key), f"census file {path} {key}")
        require(current["executions"] == expected["executions"], f"census file execution denominator drifted: {path}")
        require(
            current["passed"] + current["failed"] + current["unsupported"] == current["executions"],
            f"census file accounting drifted: {path}",
        )
    require(sum(item["executions"] for item in current_files.values()) == report["executions"], "census per-file execution accounting drifted")
    require(sum(item["passed"] for item in current_files.values()) == report["passed"], "census per-file passed accounting drifted")
    require(sum(item["failed"] for item in current_files.values()) == report["failed"], "census per-file failed accounting drifted")
    require(sum(item["unsupported"] for item in current_files.values()) == report["unsupported"], "census per-file unsupported accounting drifted")


def verify_common_report_contract(report: dict[str, Any], baseline: dict[str, Any]) -> None:
    require(report.get("schema") == REPORT_SCHEMA, "census report schema mismatch")
    for key in ("files", "tests", "executions"):
        require(report.get(key) == baseline.get(key), f"census denominator mismatch: {key}")
    for key in ("passed", "failed", "unsupported"):
        require_count(report.get(key), f"census {key}")
    require(
        report["passed"] + report["failed"] + report["unsupported"] == report["executions"],
        "census result accounting drifted",
    )
    require(report.get("exact_execution_accounting") is True, "census execution accounting is not exact")
    require(report.get("html_tokenizer_conformance_claim") is False, "census unexpectedly claims tokenizer conformance")
    require(report.get("z7_status_change") is False, "census unexpectedly changes Z7 status")
    verify_file_accounting(report, baseline)
    verify_reason_accounting(report)


def verify_exact_snapshot(report: dict[str, Any], baseline: dict[str, Any]) -> None:
    verify_common_report_contract(report, baseline)
    for key in ("passed", "failed", "unsupported"):
        require(report.get(key) == baseline.get(key), f"census exact snapshot mismatch: {key}")
    require(report.get("file_reports") == baseline.get("file_reports"), "census per-file exact snapshot drifted")
    require(report.get("reason_counts") == baseline.get("reason_counts"), "census reason exact snapshot drifted")
    require(report.get("full_corpus_pass_claim") is False, "baseline snapshot unexpectedly claims full-corpus pass")


def verify_no_regression(report: dict[str, Any], baseline: dict[str, Any]) -> None:
    verify_common_report_contract(report, baseline)
    require(report["passed"] >= baseline["passed"], f"census regression: passed fell from {baseline['passed']} to {report['passed']}")
    require(report["failed"] <= baseline["failed"], f"census regression: failed rose from {baseline['failed']} to {report['failed']}")
    require(report["unsupported"] <= baseline["unsupported"], f"census regression: unsupported rose from {baseline['unsupported']} to {report['unsupported']}")

    current_files = report["file_reports"]
    for path, expected in baseline["file_reports"].items():
        current = current_files[path]
        require(current["passed"] >= expected["passed"], f"census file regression: passed fell for {path}")
        require(current["failed"] <= expected["failed"], f"census file regression: failed rose for {path}")
        require(current["unsupported"] <= expected["unsupported"], f"census file regression: unsupported rose for {path}")

    current_reasons = report["reason_counts"]
    baseline_reasons = baseline["reason_counts"]
    all_reasons = set(current_reasons) | set(baseline_reasons)
    for reason in all_reasons:
        current = require_count(current_reasons.get(reason, 0), f"census current reason {reason!r}")
        expected = require_count(baseline_reasons.get(reason, 0), f"census baseline reason {reason!r}")
        if reason.startswith("passed:"):
            require(reason == "passed:exact-match", f"unexpected passed reason bucket: {reason}")
            require(current >= expected, f"census reason regression: {reason} fell from {expected} to {current}")
        else:
            require(current <= expected, f"census reason regression: {reason} rose from {expected} to {current}")

    require(report.get("full_corpus_pass_claim") is False, "regression gate must not manufacture full-corpus pass claim")


def verify_report_against_baseline(report: dict[str, Any], baseline: dict[str, Any], policy: str) -> None:
    require(policy in POLICIES, f"unknown census baseline policy: {policy}")
    if policy == "exact":
        verify_exact_snapshot(report, baseline)
        return
    verify_no_regression(report, baseline)


def synthetic_report(baseline: dict[str, Any]) -> dict[str, Any]:
    return {
        "schema": REPORT_SCHEMA,
        "files": baseline["files"],
        "tests": baseline["tests"],
        "executions": baseline["executions"],
        "passed": baseline["passed"],
        "failed": baseline["failed"],
        "unsupported": baseline["unsupported"],
        "file_reports": copy.deepcopy(baseline["file_reports"]),
        "reason_counts": copy.deepcopy(baseline["reason_counts"]),
        "exact_execution_accounting": True,
        "full_corpus_pass_claim": False,
        "html_tokenizer_conformance_claim": False,
        "z7_status_change": False,
    }


def expect_rejection(report: dict[str, Any], baseline: dict[str, Any], policy: str, label: str) -> None:
    try:
        verify_report_against_baseline(report, baseline, policy)
    except BaselineError:
        return
    raise BaselineError(f"self-test accepted {label}")


def one_unsupported_to_pass(report: dict[str, Any], baseline: dict[str, Any]) -> None:
    path = "tokenizer/test3.test"
    reason = "unsupported:production-fail-closed:HTML markup declaration malformed DOCTYPE-name transition is outside admitted v1 recovery"
    require(report["file_reports"][path]["unsupported"] > 0, "self-test fixture has no unsupported execution")
    require(report["reason_counts"][reason] > 0, "self-test reason has no unsupported execution")
    report["passed"] += 1
    report["unsupported"] -= 1
    report["file_reports"][path]["passed"] += 1
    report["file_reports"][path]["unsupported"] -= 1
    report["reason_counts"]["passed:exact-match"] += 1
    report["reason_counts"][reason] -= 1


def self_test() -> None:
    baseline = load_baseline(DEFAULT_BASELINE)
    exact = synthetic_report(baseline)
    verify_report_against_baseline(exact, baseline, "exact")
    verify_report_against_baseline(exact, baseline, "no-regression")

    improved = synthetic_report(baseline)
    one_unsupported_to_pass(improved, baseline)
    expect_rejection(improved, baseline, "exact", "improved result as exact snapshot")
    verify_report_against_baseline(improved, baseline, "no-regression")

    passed_to_unsupported = synthetic_report(baseline)
    path = "tokenizer/test3.test"
    passed_to_unsupported["passed"] -= 1
    passed_to_unsupported["unsupported"] += 1
    passed_to_unsupported["file_reports"][path]["passed"] -= 1
    passed_to_unsupported["file_reports"][path]["unsupported"] += 1
    passed_to_unsupported["reason_counts"]["passed:exact-match"] -= 1
    reason = "unsupported:production-fail-closed:HTML markup declaration malformed DOCTYPE-name transition is outside admitted v1 recovery"
    passed_to_unsupported["reason_counts"][reason] += 1
    expect_rejection(passed_to_unsupported, baseline, "no-regression", "passed-to-unsupported regression")

    failed_to_unsupported = synthetic_report(baseline)
    failed_to_unsupported["failed"] -= 1
    failed_to_unsupported["unsupported"] += 1
    failed_to_unsupported["file_reports"]["tokenizer/unicodeChars.test"]["failed"] -= 1
    failed_to_unsupported["file_reports"]["tokenizer/unicodeChars.test"]["unsupported"] += 1
    failed_to_unsupported["reason_counts"]["failed:parse-error-stream-mismatch"] -= 1
    failed_to_unsupported["reason_counts"]["unsupported:new-classification"] = 1
    expect_rejection(failed_to_unsupported, baseline, "no-regression", "failed-to-unsupported reclassification")

    hidden_cross_file_swap = synthetic_report(baseline)
    hidden_cross_file_swap["file_reports"]["tokenizer/test3.test"]["passed"] += 1
    hidden_cross_file_swap["file_reports"]["tokenizer/test3.test"]["unsupported"] -= 1
    hidden_cross_file_swap["file_reports"]["tokenizer/domjs.test"]["passed"] -= 1
    hidden_cross_file_swap["file_reports"]["tokenizer/domjs.test"]["unsupported"] += 1
    expect_rejection(hidden_cross_file_swap, baseline, "no-regression", "aggregate-neutral cross-file regression")

    hidden_reason_swap = synthetic_report(baseline)
    old_reason = "unsupported:production-fail-closed:HTML markup declaration malformed DOCTYPE-name transition is outside admitted v1 recovery"
    hidden_reason_swap["reason_counts"][old_reason] -= 1
    hidden_reason_swap["reason_counts"]["unsupported:new-reason"] = 1
    expect_rejection(hidden_reason_swap, baseline, "no-regression", "aggregate-neutral reason reclassification")


def main() -> int:
    parser = argparse.ArgumentParser(description="Verify the full html5lib tokenizer census against the admitted Z7 v1 diagnostic baseline")
    parser.add_argument("--probe", type=Path)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--baseline", type=Path, default=DEFAULT_BASELINE)
    parser.add_argument("--policy", choices=sorted(POLICIES), default="exact")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    try:
        require(args.baseline.resolve() == DEFAULT_BASELINE.resolve(), "baseline verifier only accepts canonical baseline path")
        require(args.manifest.resolve() == DEFAULT_MANIFEST.resolve(), "baseline verifier only accepts canonical manifest path")
        if args.self_test:
            self_test()
            print(json.dumps({"schema": BASELINE_SCHEMA, "self_test_passed": True, "policies_checked": ["exact", "no-regression"]}, sort_keys=True))
            return 0
        require(args.probe is not None, "--probe is required")
        baseline = load_baseline(args.baseline.resolve())
        report = execute_census(args.probe.resolve(), args.manifest.resolve())
        verify_report_against_baseline(report, baseline, args.policy)
    except (BaselineError, CensusError, OSError, UnicodeError, json.JSONDecodeError) as exc:
        print(f"Z7 full tokenizer census baseline verification failed: {exc}", file=sys.stderr)
        return 1

    exact_match = (
        report.get("passed") == baseline.get("passed")
        and report.get("failed") == baseline.get("failed")
        and report.get("unsupported") == baseline.get("unsupported")
        and report.get("file_reports") == baseline.get("file_reports")
        and report.get("reason_counts") == baseline.get("reason_counts")
    )
    report = {
        **report,
        "baseline_authority": BASELINE_AUTHORITY,
        "baseline_policy": args.policy,
        "baseline_match": exact_match,
        "baseline_regression_free": True,
    }
    print(json.dumps(report, sort_keys=True, separators=(",", ":"), ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
