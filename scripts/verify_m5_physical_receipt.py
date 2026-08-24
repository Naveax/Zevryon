#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys
from typing import Any

CANDIDATE_SCHEMA = "zevryon.m5.physical-candidate.v1"
MANIFEST_SCHEMA = "zevryon.m5.physical-frame-run.v1"
SHA_RE = re.compile(r"^[0-9a-fA-F]{40}$")
SHA256_RE = re.compile(r"^[0-9a-fA-F]{64}$")


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while True:
            chunk = stream.read(1024 * 1024)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def _load_object(path: Path, label: str) -> dict[str, Any]:
    if not path.is_file():
        raise ValueError(f"{label} file is missing: {path}")
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise ValueError(f"{label} is not valid JSON") from exc
    if not isinstance(value, dict):
        raise ValueError(f"{label} must be a JSON object")
    return value


def _require_true(mapping: dict[str, Any], name: str, label: str) -> None:
    if mapping.get(name) is not True:
        raise ValueError(f"{label} check is not true: {name}")


def _require_sha256(value: object, label: str) -> str:
    if not isinstance(value, str) or SHA256_RE.fullmatch(value) is None:
        raise ValueError(f"{label} is not a SHA-256 digest")
    return value.lower()


def verify_bundle(
    *,
    expected_sha: str,
    receipt_path: Path,
    manifest_path: Path,
    evidence_path: Path,
    samples_path: Path | None = None,
) -> dict[str, Any]:
    normalized_sha = expected_sha.lower()
    if SHA_RE.fullmatch(normalized_sha) is None:
        raise ValueError("expected SHA must be exactly 40 hexadecimal characters")

    receipt = _load_object(receipt_path, "candidate receipt")
    manifest = _load_object(manifest_path, "physical manifest")
    evidence = _load_object(evidence_path, "frame evidence")

    if receipt.get("schema") != CANDIDATE_SCHEMA:
        raise ValueError("candidate receipt schema mismatch")
    if manifest.get("schema") != MANIFEST_SCHEMA:
        raise ValueError("physical manifest schema mismatch")

    source = receipt.get("source")
    if not isinstance(source, dict):
        raise ValueError("candidate receipt source block is missing")
    receipt_expected = source.get("expected_sha")
    receipt_head = source.get("git_head_sha")
    if receipt_expected != normalized_sha or receipt_head != normalized_sha:
        raise ValueError("candidate receipt source SHA does not match expected SHA")
    _require_true(source, "exact_head", "candidate source")
    _require_true(source, "tracked_worktree_clean", "candidate source")

    receipt_checks = receipt.get("checks")
    if not isinstance(receipt_checks, dict):
        raise ValueError("candidate receipt checks are missing")
    for check in (
        "exact_candidate_head",
        "tracked_worktree_clean",
        "native_frame_certified",
        "manifest_embeds_exact_evidence",
        "candidate_binaries_rebuilt",
        "physical_candidate_certified",
    ):
        _require_true(receipt_checks, check, "candidate receipt")

    physical = receipt.get("physical")
    if not isinstance(physical, dict):
        raise ValueError("candidate receipt physical block is missing")
    receipt_manifest_sha = _require_sha256(
        physical.get("manifest_sha256"), "receipt manifest digest"
    )
    receipt_evidence_sha = _require_sha256(
        physical.get("evidence_sha256"), "receipt evidence digest"
    )
    actual_manifest_sha = _sha256(manifest_path)
    actual_evidence_sha = _sha256(evidence_path)
    if receipt_manifest_sha != actual_manifest_sha:
        raise ValueError("candidate receipt manifest digest mismatch")
    if receipt_evidence_sha != actual_evidence_sha:
        raise ValueError("candidate receipt evidence digest mismatch")

    artifacts = manifest.get("artifacts")
    if not isinstance(artifacts, dict):
        raise ValueError("physical manifest artifact table is missing")
    manifest_evidence_sha = _require_sha256(
        artifacts.get("evidence_sha256"), "manifest evidence digest"
    )
    if manifest_evidence_sha != actual_evidence_sha:
        raise ValueError("physical manifest evidence digest mismatch")

    embedded = manifest.get("certification")
    if embedded != evidence:
        raise ValueError("physical manifest does not embed the exact evidence object")

    evidence_checks = evidence.get("checks")
    if not isinstance(evidence_checks, dict):
        raise ValueError("frame evidence checks are missing")
    _require_true(evidence_checks, "native_frame_certified", "frame evidence")
    _require_true(evidence_checks, "frame_latency_certified", "frame evidence")

    frame_latency = evidence.get("frame_latency")
    if not isinstance(frame_latency, dict):
        raise ValueError("frame latency evidence block is missing")
    latency_checks = frame_latency.get("checks")
    if not isinstance(latency_checks, dict):
        raise ValueError("frame latency checks are missing")
    _require_true(latency_checks, "physical_metadata_complete", "frame latency")
    _require_true(latency_checks, "thermal_state_stable", "frame latency")
    _require_true(latency_checks, "frame_latency_certified", "frame latency")

    machine = frame_latency.get("machine")
    if not isinstance(machine, dict):
        raise ValueError("physical machine metadata is missing")
    if machine.get("physical_device_confirmed") is not True:
        raise ValueError("physical-device confirmation is missing")
    thermal = machine.get("thermal")
    if not isinstance(thermal, dict) or thermal.get("state") not in {"nominal", "fair"}:
        raise ValueError("physical thermal state is not admission-stable")

    samples_sha: str | None = None
    if samples_path is not None:
        if not samples_path.is_file():
            raise ValueError(f"frame samples file is missing: {samples_path}")
        samples_sha = _sha256(samples_path)
        manifest_samples_sha = _require_sha256(
            artifacts.get("samples_sha256"), "manifest samples digest"
        )
        if samples_sha != manifest_samples_sha:
            raise ValueError("frame samples digest mismatch")

    return {
        "schema": "zevryon.m5.physical-receipt-verification.v1",
        "expected_sha": normalized_sha,
        "receipt_sha256": _sha256(receipt_path),
        "manifest_sha256": actual_manifest_sha,
        "evidence_sha256": actual_evidence_sha,
        "samples_sha256": samples_sha,
        "physical_receipt_valid": True,
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Verify an M5 source-bound physical admission receipt bundle."
    )
    parser.add_argument("--expected-sha", required=True)
    parser.add_argument("--receipt", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--evidence", type=Path, required=True)
    parser.add_argument("--samples", type=Path, default=None)
    parser.add_argument("--output", type=Path, default=None)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        result = verify_bundle(
            expected_sha=args.expected_sha,
            receipt_path=args.receipt.resolve(),
            manifest_path=args.manifest.resolve(),
            evidence_path=args.evidence.resolve(),
            samples_path=args.samples.resolve() if args.samples is not None else None,
        )
        rendered = json.dumps(result, indent=2, sort_keys=True) + "\n"
        if args.output is not None:
            output = args.output.resolve()
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_text(rendered, encoding="utf-8")
        print(
            "physical_receipt_valid=true "
            f"sha={result['expected_sha']} receipt_sha256={result['receipt_sha256']}"
        )
        return 0
    except (OSError, ValueError) as exc:
        print(f"physical receipt verification failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
