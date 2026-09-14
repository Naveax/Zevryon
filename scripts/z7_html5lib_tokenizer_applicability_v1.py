#!/usr/bin/env python3
from __future__ import annotations

import argparse
import collections
import json
import sys
from pathlib import Path
from typing import Any

from z7_html5lib_tokenizer_corpus_verify import VerificationError, verify_manifest
from z7_html5lib_tokenizer_full_corpus_census_v1 import (
    EXPECTED_EXECUTIONS,
    EXPECTED_FILES,
    EXPECTED_TESTS,
    classify_pre_execution,
    expected_tokens,
    unescape_double_value,
)

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MANIFEST = ROOT / "config/z7_html5lib_tokenizer_corpus.json"
REPORT_SCHEMA = "zevryon.z7.html5lib-tokenizer-applicability.v1"
EXPECTED_APPLICABLE = 7028
EXPECTED_EXCLUDED = 8

EXPECTED_EXCLUSIONS = {
    (
        "tokenizer/unicodeCharsProblematic.test",
        0,
        "Invalid Unicode character U+DFFF",
        "Data state",
        "non-utf8-scalar-test-data",
    ),
    (
        "tokenizer/unicodeCharsProblematic.test",
        1,
        "Invalid Unicode character U+D800",
        "Data state",
        "non-utf8-scalar-test-data",
    ),
    (
        "tokenizer/unicodeCharsProblematic.test",
        2,
        "Invalid Unicode character U+DFFF with valid preceding character",
        "Data state",
        "non-utf8-scalar-test-data",
    ),
    (
        "tokenizer/unicodeCharsProblematic.test",
        3,
        "Invalid Unicode character U+D800 with valid following character",
        "Data state",
        "non-utf8-scalar-test-data",
    ),
    (
        "tokenizer/xmlViolation.test",
        0,
        "Non-XML character",
        "Data state",
        "xml-violation-infoset-coercion",
    ),
    (
        "tokenizer/xmlViolation.test",
        1,
        "Non-XML space",
        "Data state",
        "xml-violation-infoset-coercion",
    ),
    (
        "tokenizer/xmlViolation.test",
        2,
        "Double hyphen in comment",
        "Data state",
        "xml-violation-infoset-coercion",
    ),
    (
        "tokenizer/xmlViolation.test",
        3,
        "FF between attributes",
        "Data state",
        "xml-violation-infoset-coercion",
    ),
}


class ApplicabilityError(ValueError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ApplicabilityError(message)


def inspect_scope(manifest_path: Path) -> dict[str, Any]:
    try:
        provenance = verify_manifest(manifest_path, ROOT)
    except VerificationError as exc:
        raise ApplicabilityError(str(exc)) from exc

    require(provenance.get("provenance_gate_passed") is True, "corpus provenance gate failed")
    require(provenance.get("files_verified") == EXPECTED_FILES, "corpus file denominator drifted")
    require(provenance.get("tests_verified") == EXPECTED_TESTS, "corpus test denominator drifted")
    require(
        provenance.get("executions_verified") == EXPECTED_EXECUTIONS,
        "corpus execution denominator drifted",
    )
    require(provenance.get("conformance_claim") is False, "corpus provenance claim drifted")

    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    files = manifest.get("files")
    require(isinstance(files, list) and len(files) == EXPECTED_FILES, "manifest files invalid")

    executions = 0
    applicable = 0
    observed_exclusions: set[tuple[str, int, str, str, str]] = set()
    reason_counts: collections.Counter[str] = collections.Counter()

    for entry in files:
        require(isinstance(entry, dict), "manifest file entry invalid")
        upstream_path = entry.get("upstream_path")
        vendored_path = entry.get("vendored_path")
        test_array_key = entry.get("test_array_key")
        require(isinstance(upstream_path, str), "manifest upstream path invalid")
        require(isinstance(vendored_path, str), "manifest vendored path invalid")
        require(isinstance(test_array_key, str), "manifest test array key invalid")

        payload = json.loads((ROOT / vendored_path).read_text(encoding="utf-8"))
        tests = payload.get(test_array_key)
        require(isinstance(tests, list), f"{upstream_path}: test array missing")

        for test_index, raw_test in enumerate(tests):
            require(isinstance(raw_test, dict), f"{upstream_path}[{test_index}] invalid")
            description = raw_test.get("description")
            require(
                isinstance(description, str) and description,
                f"{upstream_path}[{test_index}] description invalid",
            )
            test = (
                unescape_double_value(raw_test)
                if raw_test.get("doubleEscaped", False) is True
                else raw_test
            )
            input_text = test.get("input")
            last_start_tag = test.get("lastStartTag", "")
            states = test.get("initialStates", ["Data state"])
            require(isinstance(input_text, str), f"{upstream_path}[{test_index}] input invalid")
            require(
                isinstance(last_start_tag, str),
                f"{upstream_path}[{test_index}] lastStartTag invalid",
            )
            require(
                isinstance(states, list) and states,
                f"{upstream_path}[{test_index}] initialStates invalid",
            )
            token_stream = expected_tokens(
                test,
                f"{upstream_path}[{test_index}] {description}",
            )

            for state_name in states:
                require(isinstance(state_name, str), "initial state is not string")
                executions += 1
                reason = classify_pre_execution(
                    upstream_path,
                    state_name,
                    input_text,
                    last_start_tag,
                    token_stream,
                )
                if reason is None:
                    applicable += 1
                    continue
                reason_counts[reason] += 1
                observed_exclusions.add(
                    (upstream_path, test_index, description, state_name, reason)
                )

    require(executions == EXPECTED_EXECUTIONS, "enumerated execution denominator drifted")
    require(applicable == EXPECTED_APPLICABLE, "applicable tokenizer denominator drifted")
    require(
        executions - applicable == EXPECTED_EXCLUDED,
        "excluded tokenizer denominator drifted",
    )
    require(
        observed_exclusions == EXPECTED_EXCLUSIONS,
        "tokenizer applicability exclusion identity drifted",
    )
    require(
        dict(reason_counts) == {
            "non-utf8-scalar-test-data": 4,
            "xml-violation-infoset-coercion": 4,
        },
        "tokenizer exclusion reason counts drifted",
    )

    exclusions = [
        {
            "file": item[0],
            "test_index": item[1],
            "description": item[2],
            "initial_state": item[3],
            "reason": item[4],
        }
        for item in sorted(observed_exclusions)
    ]
    return {
        "schema": REPORT_SCHEMA,
        "authority": "z7-html5lib-tokenizer-utf8-byte-input-applicability-v1",
        "upstream_commit": provenance["upstream_commit"],
        "files": EXPECTED_FILES,
        "tests": EXPECTED_TESTS,
        "executions": executions,
        "applicable_utf8_byte_input_executions": applicable,
        "excluded_non_byte_or_infoset_executions": EXPECTED_EXCLUDED,
        "reason_counts": dict(sorted(reason_counts.items())),
        "exclusions": exclusions,
        "applicability_identity_frozen": True,
        "production_utf8_byte_input_scope_complete": True,
        "full_html_tokenizer_conformance_claim": False,
        "z7_status_change": False,
    }


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Freeze the exact html5lib tokenizer executions that are representable "
            "as Zevryon UTF-8 byte input and separate XML Infoset-only expectations"
        )
    )
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    try:
        require(
            args.manifest.resolve() == DEFAULT_MANIFEST.resolve(),
            "applicability authority only accepts the canonical manifest",
        )
        report = inspect_scope(args.manifest.resolve())
        if args.self_test:
            require(
                report["applicable_utf8_byte_input_executions"] == EXPECTED_APPLICABLE,
                "self-test applicable count drifted",
            )
            require(
                len(report["exclusions"]) == EXPECTED_EXCLUDED,
                "self-test exclusion count drifted",
            )
            report = {
                "schema": "zevryon.z7.html5lib-tokenizer-applicability-self-test.v1",
                "self_test_passed": True,
                "applicable": EXPECTED_APPLICABLE,
                "excluded": EXPECTED_EXCLUDED,
            }
    except (ApplicabilityError, OSError, UnicodeError, json.JSONDecodeError) as exc:
        print(f"Z7 tokenizer applicability failed: {exc}", file=sys.stderr)
        return 1
    print(json.dumps(report, indent=2, sort_keys=True, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
