#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys
from typing import Mapping

from browser_competitor_benchmark_evidence import (
    host_metadata,
    normalized_system_fingerprint,
)
from browser_competitor_query_plan import DEFAULT_WARMUP_QUERY_COUNT
from m7_normalized_browser_full_set import (
    CanonicalNormalizedBrowserSetInvalid,
    collect_canonical_normalized_browser_report,
    validate_canonical_normalized_browser_report,
)
from m7_physical_host_evidence import (
    PhysicalHostEvidenceInvalid,
    certify_physical_host,
)


PHYSICAL_BROWSER_FULL_SET_AUTHORITY = "m7-canonical-browser-full-set-physical-stage-v1"


class PhysicalBrowserFullSetInvalid(ValueError):
    pass


def _mapping(value: object, field: str) -> Mapping[str, object]:
    if not isinstance(value, Mapping):
        raise PhysicalBrowserFullSetInvalid(f"{field} must be an object")
    return value


def attach_physical_host_evidence(
    report: Mapping[str, object],
    *,
    host_before: Mapping[str, object],
    host_after: Mapping[str, object],
) -> dict[str, object]:
    try:
        validate_canonical_normalized_browser_report(report)
    except CanonicalNormalizedBrowserSetInvalid as exc:
        raise PhysicalBrowserFullSetInvalid(
            f"normalized browser full-set evidence is invalid: {exc}"
        ) from exc

    report_host = _mapping(report.get("host"), "browser report host")
    try:
        before_receipt = certify_physical_host(
            host_before,
            label="browser-full-set-before",
        )
        after_receipt = certify_physical_host(
            host_after,
            label="browser-full-set-after",
        )
        before_fingerprint = normalized_system_fingerprint(host_before)
        after_fingerprint = normalized_system_fingerprint(host_after)
        report_fingerprint = normalized_system_fingerprint(report_host)
    except (PhysicalHostEvidenceInvalid, TypeError, ValueError) as exc:
        raise PhysicalBrowserFullSetInvalid(
            f"browser full-set physical host evidence is invalid: {exc}"
        ) from exc

    if before_fingerprint != report_fingerprint:
        raise PhysicalBrowserFullSetInvalid(
            "pre-stage physical host fingerprint differs from normalized browser collection"
        )
    if after_fingerprint != report_fingerprint:
        raise PhysicalBrowserFullSetInvalid(
            "post-stage physical host fingerprint differs from normalized browser collection"
        )

    output = dict(report)
    output["physical_browser_full_set_authority"] = PHYSICAL_BROWSER_FULL_SET_AUTHORITY
    output["host_before"] = dict(host_before)
    output["host_after"] = dict(host_after)
    output["physical_host_evidence"] = {
        "before": before_receipt,
        "after": after_receipt,
        "physical_host_gate_passed": True,
    }
    return output


def validate_physical_browser_full_set(report: Mapping[str, object]) -> None:
    if report.get("physical_browser_full_set_authority") != PHYSICAL_BROWSER_FULL_SET_AUTHORITY:
        raise PhysicalBrowserFullSetInvalid(
            "browser full-set physical-stage authority mismatch"
        )
    host_before = _mapping(report.get("host_before"), "host_before")
    host_after = _mapping(report.get("host_after"), "host_after")
    recomputed = attach_physical_host_evidence(
        report,
        host_before=host_before,
        host_after=host_after,
    )
    embedded = _mapping(report.get("physical_host_evidence"), "physical_host_evidence")
    if embedded != recomputed["physical_host_evidence"]:
        raise PhysicalBrowserFullSetInvalid(
            "embedded browser full-set physical host receipts drifted from raw M0 evidence"
        )


def collect_physical_browser_full_set(
    *,
    payload_bytes: int,
    query_count: int,
    warmup_query_count: int,
    virtual_slice_bytes: int,
    virtual_timeout_seconds: int,
    native_timeout_seconds: int,
) -> dict[str, object]:
    host_before = host_metadata()
    try:
        certify_physical_host(host_before, label="browser-full-set-before")
    except PhysicalHostEvidenceInvalid as exc:
        raise PhysicalBrowserFullSetInvalid(
            f"pre-stage physical host certification failed: {exc}"
        ) from exc

    report = collect_canonical_normalized_browser_report(
        payload_bytes=payload_bytes,
        query_count=query_count,
        warmup_query_count=warmup_query_count,
        virtual_slice_bytes=virtual_slice_bytes,
        virtual_timeout_seconds=virtual_timeout_seconds,
        native_timeout_seconds=native_timeout_seconds,
    )
    host_after = host_metadata()
    output = attach_physical_host_evidence(
        _mapping(report, "normalized browser full-set report"),
        host_before=host_before,
        host_after=host_after,
    )
    validate_physical_browser_full_set(output)
    return output


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Collect the canonical M7 six-browser x two-mode normalized full set with "
            "certified M0 physical/thermal receipts immediately before and after the "
            "entire timed browser stage."
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
        report = collect_physical_browser_full_set(
            payload_bytes=args.payload_bytes,
            query_count=args.query_count,
            warmup_query_count=args.warmup_query_count,
            virtual_slice_bytes=args.virtual_slice_bytes,
            virtual_timeout_seconds=args.virtual_timeout_seconds,
            native_timeout_seconds=args.native_timeout_seconds,
        )
    except (
        ValueError,
        CanonicalNormalizedBrowserSetInvalid,
        PhysicalBrowserFullSetInvalid,
    ) as exc:
        print(f"M7 physical browser full-set failed: {exc}", file=sys.stderr)
        return 1

    text = json.dumps(report, indent=2, sort_keys=True) + "\n"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(text, encoding="utf-8")
    print(text, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
