#!/usr/bin/env python3
from __future__ import annotations

import argparse
from datetime import datetime, timezone
from pathlib import Path
import sys

from m8_bundle_common import (
    RECEIPT_SCHEMA,
    EvidenceInvalid,
    artifact_path,
    authority_source_hashes,
    canonical_json,
    clean_git_identity,
    exclusive_write,
    load_bundle_plan,
    receipt_path,
    sha256_bytes,
)
from m8_profile_observation_gate import (
    EvidenceInvalid as ProfileEvidenceInvalid,
    evaluate_document,
    load_json_strict,
)


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Import one raw four-profile observation artifact into a frozen M8 bundle exactly once."
    )
    parser.add_argument("--artifact-root", type=Path, required=True)
    parser.add_argument("--input", type=Path, required=True)
    args = parser.parse_args()

    try:
        root = args.artifact_root.resolve()
        plan, _ = load_bundle_plan(root, verify_current=True)
        destination = artifact_path(root, plan, "profile")
        receipt = receipt_path(root, plan, "profile")
        if destination.exists():
            raise EvidenceInvalid(f"profile artifact already exists and may not be replaced: {destination}")
        if receipt.exists():
            raise EvidenceInvalid(f"profile import is already sealed by receipt: {receipt}")

        source = args.input.resolve()
        raw = source.read_bytes()
        source_sha = sha256_bytes(raw)
        evidence_valid = False
        gate_passed = False
        error: str | None = None
        report = None
        try:
            document = load_json_strict(raw)
            report, gate_passed = evaluate_document(document, raw)
            if report["candidate_commit"] != plan["candidate_commit"]:
                raise ProfileEvidenceInvalid("profile candidate_commit does not match frozen bundle candidate")
            if report["candidate_tree"] != plan["candidate_tree"]:
                raise ProfileEvidenceInvalid("profile candidate_tree does not match frozen bundle candidate")
            evidence_valid = True
        except (ProfileEvidenceInvalid, KeyError, TypeError, ValueError) as exc:
            error = str(exc)

        destination_sha = None
        if evidence_valid:
            destination.parent.mkdir(parents=True, exist_ok=True)
            try:
                with destination.open("xb") as handle:
                    handle.write(raw)
            except FileExistsError as exc:
                raise EvidenceInvalid(f"refusing to overwrite immutable profile artifact: {destination}") from exc
            destination_sha = sha256_bytes(raw)

        current_commit, current_tree = clean_git_identity()
        candidate_unchanged = (
            current_commit == plan["candidate_commit"] and current_tree == plan["candidate_tree"]
        )
        sources_unchanged = authority_source_hashes() == plan["authority_source_sha256"]
        receipt_document = {
            "schema": RECEIPT_SCHEMA,
            "authority": "m8-single-use-profile-import-v1",
            "kind": "profile-import-v1",
            "bundle_id": plan["bundle_id"],
            "candidate_commit": plan["candidate_commit"],
            "candidate_tree": plan["candidate_tree"],
            "artifact_key": "profile",
            "artifact_path": plan["artifacts"]["profile"],
            "receipt_path": plan["receipts"]["profile"],
            "imported_utc": utc_now(),
            "source_path": str(source),
            "source_sha256": source_sha,
            "artifact_exists": destination.is_file(),
            "artifact_sha256": destination_sha,
            "artifact_bytes": len(raw) if evidence_valid else None,
            "candidate_unchanged": candidate_unchanged,
            "authority_sources_unchanged": sources_unchanged,
            "evidence_valid": evidence_valid,
            "recomputed_gate_passed": gate_passed if evidence_valid else False,
            "recomputed_profile_gate": report,
            "error": error,
        }
        exclusive_write(receipt, canonical_json(receipt_document))
    except (EvidenceInvalid, OSError) as exc:
        print(f"M8 profile bundle import failed: {exc}", file=sys.stderr)
        return 1

    if not evidence_valid or not candidate_unchanged or not sources_unchanged:
        return 1
    return 0 if gate_passed else 2


if __name__ == "__main__":
    raise SystemExit(main())
