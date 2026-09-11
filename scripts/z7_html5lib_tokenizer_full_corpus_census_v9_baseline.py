#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

from z7_html5lib_tokenizer_full_corpus_census_v1 import CensusError, DEFAULT_MANIFEST, execute_census
from z7_html5lib_tokenizer_full_corpus_census_v1_baseline import BaselineError, verify_report_against_baseline

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BASELINE = ROOT / "config/z7_html5lib_tokenizer_full_corpus_census_v9.json"
BASELINE_SCHEMA = "zevryon.z7.html5lib-tokenizer-full-corpus-census-baseline.v9"
BASELINE_AUTHORITY = "z7-html5lib-tokenizer-full-corpus-census-v9"
PINNED_UPSTREAM_COMMIT = "224991ec10db04f056a89eed8b0bd8695fd2950e"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise BaselineError(message)


def load_baseline(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    require(isinstance(value, dict), "census v9 baseline root must be object")
    require(value.get("schema") == BASELINE_SCHEMA, "census v9 baseline schema mismatch")
    require(value.get("authority") == BASELINE_AUTHORITY, "census v9 baseline authority mismatch")
    require(value.get("upstream_commit") == PINNED_UPSTREAM_COMMIT, "census v9 upstream commit drifted")
    require(value.get("files") == 14, "census v9 file denominator drifted")
    require(value.get("tests") == 6810, "census v9 test denominator drifted")
    require(value.get("executions") == 7036, "census v9 execution denominator drifted")
    require(value.get("passed") == 6775, "census v9 passed denominator drifted")
    require(value.get("failed") == 122, "census v9 failed denominator drifted")
    require(value.get("unsupported") == 139, "census v9 unsupported denominator drifted")
    require(value["passed"] + value["failed"] + value["unsupported"] == value["executions"], "census v9 accounting drifted")
    require(value.get("source_run_id") == 34599262279, "census v9 source run drifted")
    require(value.get("source_head_sha") == "27ccbdd9258ad44deb79e53c0c2c8ee1cbbe5ecc", "census v9 source head drifted")
    require(value.get("full_corpus_pass_claim") is False, "census v9 must not claim full pass")
    require(value.get("html_tokenizer_conformance_claim") is False, "census v9 must not claim tokenizer conformance")
    require(value.get("z7_status_change") is False, "census v9 must not change Z7 status")
    require(isinstance(value.get("file_reports"), dict), "census v9 file_reports must be object")
    require(isinstance(value.get("reason_counts"), dict), "census v9 reason_counts must be object")
    require(value["reason_counts"].get("unsupported:input-preprocessing-nul") == 67, "census v9 NUL debt drifted")
    require("unsupported:input-preprocessing-cr" not in value["reason_counts"], "census v9 must not retain CR preprocessing debt")
    return value


def main() -> int:
    parser = argparse.ArgumentParser(description="Verify the full html5lib tokenizer census against the post-CRLF Z7 v9 baseline")
    parser.add_argument("--probe", type=Path)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--baseline", type=Path, default=DEFAULT_BASELINE)
    parser.add_argument("--policy", choices=("exact", "no-regression"), default="exact")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    try:
        require(args.baseline.resolve() == DEFAULT_BASELINE.resolve(), "v9 verifier only accepts canonical baseline path")
        require(args.manifest.resolve() == DEFAULT_MANIFEST.resolve(), "v9 verifier only accepts canonical manifest path")
        baseline = load_baseline(args.baseline.resolve())
        if args.self_test:
            synthetic = {
                "schema": "zevryon.z7.html5lib-tokenizer-full-corpus-census.v1",
                "files": baseline["files"], "tests": baseline["tests"], "executions": baseline["executions"],
                "passed": baseline["passed"], "failed": baseline["failed"], "unsupported": baseline["unsupported"],
                "file_reports": baseline["file_reports"], "reason_counts": baseline["reason_counts"],
                "exact_execution_accounting": True, "full_corpus_pass_claim": False,
                "html_tokenizer_conformance_claim": False, "z7_status_change": False,
            }
            verify_report_against_baseline(synthetic, baseline, "exact")
            verify_report_against_baseline(synthetic, baseline, "no-regression")
            print(json.dumps({"schema": BASELINE_SCHEMA, "authority": BASELINE_AUTHORITY, "self_test_passed": True}, sort_keys=True))
            return 0
        require(args.probe is not None, "--probe is required")
        report = execute_census(args.probe.resolve(), args.manifest.resolve())
        verify_report_against_baseline(report, baseline, args.policy)
        exact_match = report.get("file_reports") == baseline.get("file_reports") and report.get("reason_counts") == baseline.get("reason_counts")
        output = {**report, "baseline_authority": BASELINE_AUTHORITY, "baseline_policy": args.policy, "baseline_match": exact_match, "baseline_regression_free": True}
        print(json.dumps(output, sort_keys=True, separators=(",", ":"), ensure_ascii=False))
        return 0
    except (BaselineError, CensusError, OSError, UnicodeError, json.JSONDecodeError) as exc:
        print(f"Z7 tokenizer census v9 baseline verification failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
