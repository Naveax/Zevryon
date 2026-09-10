#!/usr/bin/env python3
from __future__ import annotations

import argparse
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


class BaselineError(ValueError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise BaselineError(message)


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
    require(value.get("full_corpus_pass_claim") is False, "baseline must not claim full-corpus pass")
    require(value.get("html_tokenizer_conformance_claim") is False, "baseline must not claim tokenizer conformance")
    require(value.get("z7_status_change") is False, "baseline must not change Z7 status")
    return value


def verify_report_against_baseline(report: dict[str, Any], baseline: dict[str, Any]) -> None:
    require(report.get("schema") == REPORT_SCHEMA, "census report schema mismatch")
    for key in ("files", "tests", "executions", "passed", "failed", "unsupported"):
        require(report.get(key) == baseline.get(key), f"census baseline mismatch: {key}")
    require(report.get("file_reports") == baseline.get("file_reports"), "census per-file baseline drifted")
    require(report.get("reason_counts") == baseline.get("reason_counts"), "census reason baseline drifted")
    require(report.get("exact_execution_accounting") is True, "census execution accounting is not exact")
    require(report.get("full_corpus_pass_claim") is False, "census unexpectedly claims full-corpus pass")
    require(report.get("html_tokenizer_conformance_claim") is False, "census unexpectedly claims tokenizer conformance")
    require(report.get("z7_status_change") is False, "census unexpectedly changes Z7 status")


def self_test() -> None:
    baseline = load_baseline(DEFAULT_BASELINE)
    synthetic = {
        "schema": REPORT_SCHEMA,
        "files": baseline["files"],
        "tests": baseline["tests"],
        "executions": baseline["executions"],
        "passed": baseline["passed"],
        "failed": baseline["failed"],
        "unsupported": baseline["unsupported"],
        "file_reports": baseline["file_reports"],
        "reason_counts": baseline["reason_counts"],
        "exact_execution_accounting": True,
        "full_corpus_pass_claim": False,
        "html_tokenizer_conformance_claim": False,
        "z7_status_change": False,
    }
    verify_report_against_baseline(synthetic, baseline)
    drifted = dict(synthetic)
    drifted["passed"] += 1
    try:
        verify_report_against_baseline(drifted, baseline)
    except BaselineError:
        pass
    else:
        raise BaselineError("self-test accepted census count drift")


def main() -> int:
    parser = argparse.ArgumentParser(description="Verify the full html5lib tokenizer census against the admitted Z7 v1 diagnostic baseline")
    parser.add_argument("--probe", type=Path)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--baseline", type=Path, default=DEFAULT_BASELINE)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    try:
        require(args.baseline.resolve() == DEFAULT_BASELINE.resolve(), "baseline verifier only accepts canonical baseline path")
        require(args.manifest.resolve() == DEFAULT_MANIFEST.resolve(), "baseline verifier only accepts canonical manifest path")
        if args.self_test:
            self_test()
            print(json.dumps({"schema": BASELINE_SCHEMA, "self_test_passed": True}, sort_keys=True))
            return 0
        require(args.probe is not None, "--probe is required")
        baseline = load_baseline(args.baseline.resolve())
        report = execute_census(args.probe.resolve(), args.manifest.resolve())
        verify_report_against_baseline(report, baseline)
    except (BaselineError, CensusError, OSError, UnicodeError, json.JSONDecodeError) as exc:
        print(f"Z7 full tokenizer census baseline verification failed: {exc}", file=sys.stderr)
        return 1
    report = {**report, "baseline_authority": BASELINE_AUTHORITY, "baseline_match": True}
    print(json.dumps(report, sort_keys=True, separators=(",", ":"), ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
