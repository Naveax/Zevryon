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
from m7_physical_host_evidence import (
    PhysicalHostEvidenceInvalid,
    certify_physical_host,
)
import m7_zevryon_normalized_case as normalized_case


PHYSICAL_CASE_AUTHORITY = "m7-zevryon-normalized-physical-case-v1"


class PhysicalZevryonCaseInvalid(ValueError):
    pass


def _mapping(value: object, field: str) -> Mapping[str, object]:
    if not isinstance(value, Mapping):
        raise PhysicalZevryonCaseInvalid(f"{field} must be an object")
    return value


def _mode(value: object) -> str:
    if value not in {"virtualized", "native-dom"}:
        raise PhysicalZevryonCaseInvalid("normalized Zevryon record has invalid mode")
    return str(value)


def attach_physical_host_evidence(
    record: Mapping[str, object],
    *,
    host_before: Mapping[str, object],
    host_after: Mapping[str, object],
) -> dict[str, object]:
    mode = _mode(record.get("mode"))
    before_label = f"zevryon-{mode}-before"
    after_label = f"zevryon-{mode}-after"
    try:
        before_receipt = certify_physical_host(host_before, label=before_label)
        after_receipt = certify_physical_host(host_after, label=after_label)
        before_fingerprint = normalized_system_fingerprint(host_before)
        after_fingerprint = normalized_system_fingerprint(host_after)
    except (PhysicalHostEvidenceInvalid, TypeError, ValueError) as exc:
        raise PhysicalZevryonCaseInvalid(
            f"Zevryon physical host evidence is invalid: {exc}"
        ) from exc

    record_fingerprint = record.get("system_fingerprint")
    if not isinstance(record_fingerprint, str) or not record_fingerprint:
        raise PhysicalZevryonCaseInvalid(
            "normalized Zevryon record lacks system_fingerprint"
        )
    if before_fingerprint != record_fingerprint:
        raise PhysicalZevryonCaseInvalid(
            "pre-case physical host fingerprint differs from normalized measurement"
        )
    if after_fingerprint != record_fingerprint:
        raise PhysicalZevryonCaseInvalid(
            "post-case physical host fingerprint differs from normalized measurement"
        )
    if record.get("host_platform") != host_before.get("platform"):
        raise PhysicalZevryonCaseInvalid(
            "normalized Zevryon host platform differs from physical host receipt"
        )
    if record.get("host_arch") != host_before.get("arch"):
        raise PhysicalZevryonCaseInvalid(
            "normalized Zevryon host architecture differs from physical host receipt"
        )

    output = dict(record)
    output["physical_case_authority"] = PHYSICAL_CASE_AUTHORITY
    output["host"] = dict(host_before)
    output["host_after"] = dict(host_after)
    output["physical_host_evidence"] = {
        "before": before_receipt,
        "after": after_receipt,
        "physical_host_gate_passed": True,
    }
    return output


def run_physical_case(
    *,
    session_binary: Path,
    mode: str,
    payload_bytes: int,
    query_count: int,
    warmup_query_count: int,
    virtual_slice_bytes: int,
    timeout_seconds: int,
    max_fragments: int = 512,
) -> dict[str, object]:
    if mode not in {"virtualized", "native-dom"}:
        raise PhysicalZevryonCaseInvalid(f"unknown benchmark mode: {mode}")
    host_before = host_metadata()
    try:
        certify_physical_host(
            host_before,
            label=f"zevryon-{mode}-before",
        )
    except PhysicalHostEvidenceInvalid as exc:
        raise PhysicalZevryonCaseInvalid(
            f"pre-case physical host certification failed: {exc}"
        ) from exc

    record = normalized_case.run_normalized_case(
        session_binary=session_binary,
        mode=mode,
        payload_bytes=payload_bytes,
        query_count=query_count,
        warmup_query_count=warmup_query_count,
        virtual_slice_bytes=virtual_slice_bytes,
        timeout_seconds=timeout_seconds,
        max_fragments=max_fragments,
    )
    host_after = host_metadata()
    return attach_physical_host_evidence(
        _mapping(record, "normalized Zevryon record"),
        host_before=host_before,
        host_after=host_after,
    )


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Collect canonical physical-host M7 Zevryon evidence around the admitted "
            "normalized persistent-session collector. Physical-device confirmation and "
            "observed thermal evidence are required before and after the timed case."
        )
    )
    parser.add_argument("--session-binary", type=Path, required=True)
    parser.add_argument(
        "--mode",
        choices=("virtualized", "native-dom"),
        required=True,
    )
    parser.add_argument("--payload-bytes", type=int, default=64 * 1024 * 1024)
    parser.add_argument("--query-count", type=int, default=21)
    parser.add_argument(
        "--warmup-query-count",
        type=int,
        default=DEFAULT_WARMUP_QUERY_COUNT,
    )
    parser.add_argument("--virtual-slice-bytes", type=int, default=128 * 1024)
    parser.add_argument("--timeout-seconds", type=int, required=True)
    parser.add_argument("--max-fragments", type=int, default=512)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    try:
        record = run_physical_case(
            session_binary=args.session_binary,
            mode=args.mode,
            payload_bytes=args.payload_bytes,
            query_count=args.query_count,
            warmup_query_count=args.warmup_query_count,
            virtual_slice_bytes=args.virtual_slice_bytes,
            timeout_seconds=args.timeout_seconds,
            max_fragments=args.max_fragments,
        )
    except (
        OSError,
        normalized_case.ProcessScopeUnavailable,
        normalized_case.ZevryonNormalizedCaseInvalid,
        normalized_case.ZevryonNormalizedCaseTimeout,
        PhysicalZevryonCaseInvalid,
    ) as exc:
        print(f"M7 Zevryon physical case failed: {exc}", file=sys.stderr)
        return 1

    text = json.dumps(record, indent=2, sort_keys=True) + "\n"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(text, encoding="utf-8")
    print(text, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
