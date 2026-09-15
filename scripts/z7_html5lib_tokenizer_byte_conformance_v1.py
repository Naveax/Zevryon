#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from z7_html5lib_tokenizer_applicability_v1 import (
    DEFAULT_MANIFEST,
    EXPECTED_APPLICABLE,
    EXPECTED_EXCLUDED,
    ApplicabilityError,
    inspect_scope,
)
from z7_html5lib_tokenizer_full_corpus_census_v1 import CensusError, execute_census

ROOT = Path(__file__).resolve().parents[1]
REPORT_SCHEMA = "zevryon.z7.html5lib-tokenizer-byte-conformance.v1"


class BinderError(ValueError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise BinderError(message)


def bind(probe: Path, manifest: Path) -> dict[str, object]:
    applicability = inspect_scope(manifest)
    census = execute_census(probe, manifest)

    require(census["files"] == 14, "census file denominator drifted")
    require(census["tests"] == 6810, "census test denominator drifted")
    require(census["executions"] == 7036, "census execution denominator drifted")
    require(census["passed"] == EXPECTED_APPLICABLE, "applicable production pass count drifted")
    require(census["failed"] == 0, "production tokenizer has failing applicable executions")
    require(census["unsupported"] == EXPECTED_EXCLUDED, "census exclusion count drifted")
    require(
        census["passed"] + census["failed"] + census["unsupported"] == census["executions"],
        "census accounting is not exact",
    )
    require(
        applicability["applicable_utf8_byte_input_executions"] == census["passed"],
        "applicability/pass denominator mismatch",
    )
    require(
        applicability["excluded_non_byte_or_infoset_executions"] == census["unsupported"],
        "applicability/unsupported denominator mismatch",
    )

    reason_counts = census["reason_counts"]
    require(
        reason_counts.get("unsupported:non-utf8-scalar-test-data") == 4,
        "isolated-surrogate exclusion count drifted",
    )
    require(
        reason_counts.get("unsupported:xml-violation-infoset-coercion") == 4,
        "XML Infoset exclusion count drifted",
    )
    require(
        sum(
            count
            for reason, count in reason_counts.items()
            if reason.startswith("unsupported:")
        ) == EXPECTED_EXCLUDED,
        "unexpected tokenizer exclusion class appeared",
    )
    require(
        sum(
            count
            for reason, count in reason_counts.items()
            if reason.startswith("failed:")
        ) == 0,
        "unexpected tokenizer failure class appeared",
    )

    return {
        "schema": REPORT_SCHEMA,
        "authority": "z7-html-tokenizer-native-utf8-byte-conformance-v1",
        "conformance_scope": "production-valid-utf8-byte-input-tokenizer",
        "upstream_commit": applicability["upstream_commit"],
        "files": census["files"],
        "tests": census["tests"],
        "executions": census["executions"],
        "applicable_executions": census["passed"],
        "applicable_passed": census["passed"],
        "applicable_failed": census["failed"],
        "scope_excluded_executions": census["unsupported"],
        "scope_exclusion_reason_counts": {
            "isolated-surrogate-abstract-input": 4,
            "xml-infoset-coercion-not-tokenization": 4,
        },
        "scope_exclusion_identity_frozen": applicability["applicability_identity_frozen"],
        "html_tokenizer_conformance_claim": True,
        "full_html5lib_abstract_input_conformance_claim": False,
        "tree_builder_conformance_claim": False,
        "z7_status_change": False,
    }


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Bind exact html5lib census evidence to Zevryon's native UTF-8 byte tokenizer scope"
    )
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    args = parser.parse_args()
    try:
        probe = args.probe.resolve()
        manifest = args.manifest.resolve()
        require(probe.is_file(), f"tokenizer probe missing: {probe}")
        require(manifest == DEFAULT_MANIFEST.resolve(), "binder requires canonical manifest")
        report = bind(probe, manifest)
    except (
        BinderError,
        ApplicabilityError,
        CensusError,
        OSError,
        UnicodeError,
        json.JSONDecodeError,
    ) as exc:
        print(f"Z7 tokenizer byte-conformance binder failed: {exc}", file=sys.stderr)
        return 1
    print(json.dumps(report, indent=2, sort_keys=True, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
