#!/usr/bin/env python3
from __future__ import annotations

import argparse
import collections
import json
import sys
from pathlib import Path
from typing import Any

from z7_wpt_tree_corpus_verify import VerificationError, verify_manifest
from z7_wpt_tree_runner_v1 import RunnerError, execute_probe, parse_fixture

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MANIFEST = ROOT / "config/z7_wpt_tree_corpus.json"
REPORT_SCHEMA = "zevryon.z7.wpt-tree-structural-census.v1"
MAX_SAMPLES = 20


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RunnerError(message)


def add_sample(samples: list[dict[str, Any]], value: dict[str, Any]) -> None:
    if len(samples) < MAX_SAMPLES:
        samples.append(value)


def run(probe: Path, manifest_path: Path) -> dict[str, Any]:
    require(probe.is_file(), f"tree probe missing: {probe}")
    try:
        provenance = verify_manifest(manifest_path, ROOT)
    except VerificationError as exc:
        raise RunnerError(str(exc)) from exc
    require(provenance.get("provenance_gate_passed") is True, "WPT tree provenance gate failed")
    require(provenance.get("conformance_claim") is False, "WPT corpus unexpectedly claims conformance")

    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    totals: collections.Counter[str] = collections.Counter()
    reasons: collections.Counter[str] = collections.Counter()
    samples: list[dict[str, Any]] = []
    tests = 0

    for entry in manifest["files"]:
        cases = parse_fixture(ROOT / entry["vendored_path"], entry["upstream_path"])
        tests += len(cases)
        for case in cases:
            for scripting in case.scripting_modes:
                totals["executions"] += 1
                if case.fragment_context is not None:
                    totals["unsupported"] += 1
                    reasons["document-fragment-context-not-exposed"] += 1
                    add_sample(samples, {
                        "file": case.source_path,
                        "test_index": case.index,
                        "scripting": scripting,
                        "status": "unsupported",
                        "reason": "document-fragment-context-not-exposed",
                    })
                    continue

                status, reason, actual_tree, capabilities = execute_probe(probe, case, scripting)
                if status != "ok":
                    totals[status] += 1
                    reasons[reason] += 1
                    add_sample(samples, {
                        "file": case.source_path,
                        "test_index": case.index,
                        "scripting": scripting,
                        "status": status,
                        "reason": reason,
                    })
                    continue

                assert actual_tree is not None and capabilities is not None
                if actual_tree == case.document:
                    totals["structural_matches"] += 1
                    reasons["exact-tree-match"] += 1
                else:
                    totals["structural_mismatches"] += 1
                    reasons["tree-dump-mismatch"] += 1
                    add_sample(samples, {
                        "file": case.source_path,
                        "test_index": case.index,
                        "scripting": scripting,
                        "status": "structural-mismatch",
                        "reason": "tree-dump-mismatch",
                        "expected": list(case.document[:8]),
                        "actual": list(actual_tree[:8]),
                    })

                if case.errors and not capabilities.get("parse-errors", False):
                    totals["conformance_blocked"] += 1
                    reasons["parse-error-stream-authority-not-exposed"] += 1
                if not capabilities.get("namespaces", False) and any(
                    "math " in line or "svg " in line for line in case.document
                ):
                    totals["conformance_blocked"] += 1
                    reasons["namespace-tree-authority-not-exposed"] += 1

    require(totals["executions"] == provenance["executions_verified"],
            "structural census execution denominator drifted")
    classified = totals["structural_matches"] + totals["structural_mismatches"] + totals["unsupported"] + totals["failed"]
    require(classified == totals["executions"], "structural census classification denominator drifted")

    return {
        "schema": REPORT_SCHEMA,
        "authority": "z7-wpt-tree-structural-census-v1",
        "upstream_commit": provenance["upstream_commit"],
        "files": provenance["files_verified"],
        "tests": tests,
        "executions": totals["executions"],
        "structural_matches": totals["structural_matches"],
        "structural_mismatches": totals["structural_mismatches"],
        "unsupported": totals["unsupported"],
        "failed": totals["failed"],
        "conformance_blocked": totals["conformance_blocked"],
        "reason_counts": dict(sorted(reasons.items())),
        "samples": samples,
        "production_probe": str(probe),
        "structural_only": True,
        "tree_builder_conformance_claim": False,
        "z7_status_change": False,
    }


def self_test(manifest_path: Path) -> dict[str, Any]:
    try:
        provenance = verify_manifest(manifest_path, ROOT)
    except VerificationError as exc:
        raise RunnerError(str(exc)) from exc
    require(provenance["tests_verified"] > 0, "structural census self-test requires cases")
    require(provenance["executions_verified"] >= provenance["tests_verified"],
            "structural census self-test denominator invalid")
    return {
        "schema": "zevryon.z7.wpt-tree-structural-census-self-test.v1",
        "tests_verified": provenance["tests_verified"],
        "executions_verified": provenance["executions_verified"],
        "self_test_passed": True,
        "tree_builder_conformance_claim": False,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Measure exact tree structure independently of missing parse-error authority")
    parser.add_argument("--probe", type=Path)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    try:
        if args.self_test:
            report = self_test(args.manifest)
        else:
            require(args.probe is not None, "--probe is required unless --self-test is used")
            report = run(args.probe, args.manifest)
    except RunnerError as exc:
        print(f"Z7 WPT tree structural census error: {exc}", file=sys.stderr)
        return 2
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
