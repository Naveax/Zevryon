#!/usr/bin/env python3
from __future__ import annotations

import argparse
from datetime import datetime, timezone
from pathlib import Path
import secrets
import sys

from m8_bundle_common import (
    ARTIFACT_PATHS,
    FINAL_OUTPUT_PATH,
    PLAN_AUTHORITY,
    PLAN_FILENAME,
    PLAN_SCHEMA,
    RECEIPT_PATHS,
    EvidenceInvalid,
    authority_source_hashes,
    canonical_json,
    clean_git_identity,
    exclusive_write,
    resolve_contained,
    source_root,
    validate_bundle_plan,
)


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Freeze one immutable M8 certification bundle before collecting long-running evidence."
    )
    parser.add_argument("--artifact-root", type=Path, required=True)
    parser.add_argument("--bundle-id", default=None, help="Optional explicit lowercase 32-hex bundle id for controlled tests.")
    args = parser.parse_args()

    try:
        root = args.artifact_root.resolve()
        repository = source_root().resolve()
        if root == repository or root.is_relative_to(repository):
            raise EvidenceInvalid("artifact root must be outside the repository")
        if root.exists() and any(root.iterdir()):
            raise EvidenceInvalid("artifact root must be absent or empty when the bundle is frozen")
        root.mkdir(parents=True, exist_ok=True)

        commit, tree = clean_git_identity()
        bundle_id = args.bundle_id if args.bundle_id is not None else secrets.token_hex(16)
        plan = {
            "schema": PLAN_SCHEMA,
            "authority": PLAN_AUTHORITY,
            "bundle_id": bundle_id,
            "candidate_commit": commit,
            "candidate_tree": tree,
            "created_utc": utc_now(),
            "artifacts": dict(ARTIFACT_PATHS),
            "receipts": dict(RECEIPT_PATHS),
            "final_output": FINAL_OUTPUT_PATH,
            "authority_source_sha256": authority_source_hashes(),
        }
        validate_bundle_plan(plan, root, verify_current=True)
        plan_path = resolve_contained(root, PLAN_FILENAME, "bundle plan")
        text = canonical_json(plan)
        exclusive_write(plan_path, text)
    except (EvidenceInvalid, OSError) as exc:
        print(f"M8 bundle freeze failed: {exc}", file=sys.stderr)
        return 1

    print(text, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
