#!/usr/bin/env python3
from __future__ import annotations

import argparse
from datetime import datetime, timezone
from pathlib import Path
import subprocess
import sys
import time

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
    sha256_file,
)

RUNNABLE_KEYS = {"storage_crash", "mixed_mutation", "soak", "property_fuzz"}


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")


def command_file_hashes(command: list[str]) -> dict[str, str]:
    hashes: dict[str, str] = {}
    for argument in command:
        candidate = Path(argument)
        try:
            if candidate.is_file():
                resolved = str(candidate.resolve())
                hashes[resolved] = sha256_file(candidate.resolve())
        except OSError:
            continue
    return hashes


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run one M8 authority exactly once inside a pre-frozen certification bundle."
    )
    parser.add_argument("--artifact-root", type=Path, required=True)
    parser.add_argument("--artifact-key", choices=sorted(RUNNABLE_KEYS), required=True)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()

    try:
        root = args.artifact_root.resolve()
        plan, _ = load_bundle_plan(root, verify_current=True)
        key = args.artifact_key
        output = artifact_path(root, plan, key)
        receipt = receipt_path(root, plan, key)
        if output.exists():
            raise EvidenceInvalid(f"artifact already exists and may not be replaced: {output}")
        if receipt.exists():
            raise EvidenceInvalid(f"artifact invocation is already sealed by receipt: {receipt}")

        command = list(args.command)
        if command and command[0] == "--":
            command = command[1:]
        if not command:
            raise EvidenceInvalid("authority command is required after --")
        placeholder_count = sum(argument.count("{artifact}") for argument in command)
        if placeholder_count != 1:
            raise EvidenceInvalid("authority command must contain {artifact} exactly once")
        command = [
            argument.replace("{artifact}", str(output))
            .replace("{artifact_root}", str(root))
            .replace("{bundle_id}", plan["bundle_id"])
            for argument in command
        ]
        file_hashes_before = command_file_hashes(command)
        output.parent.mkdir(parents=True, exist_ok=True)

        started_utc = utc_now()
        started_ns = time.monotonic_ns()
        try:
            completed = subprocess.run(command, check=False)
            returncode = int(completed.returncode)
        except OSError as exc:
            returncode = 127
            launch_error = str(exc)
        else:
            launch_error = None
        ended_ns = time.monotonic_ns()
        ended_utc = utc_now()

        artifact_exists = output.is_file()
        artifact_sha = sha256_file(output) if artifact_exists else None
        artifact_bytes = output.stat().st_size if artifact_exists else None

        try:
            current_commit, current_tree = clean_git_identity()
            candidate_unchanged = (
                current_commit == plan["candidate_commit"] and current_tree == plan["candidate_tree"]
            )
            sources_unchanged = authority_source_hashes() == plan["authority_source_sha256"]
        except EvidenceInvalid:
            candidate_unchanged = False
            sources_unchanged = False

        receipt_document = {
            "schema": RECEIPT_SCHEMA,
            "authority": "m8-single-use-bundle-authority-runner-v1",
            "kind": "authority-invocation-v1",
            "bundle_id": plan["bundle_id"],
            "candidate_commit": plan["candidate_commit"],
            "candidate_tree": plan["candidate_tree"],
            "artifact_key": key,
            "artifact_path": plan["artifacts"][key],
            "receipt_path": plan["receipts"][key],
            "started_utc": started_utc,
            "ended_utc": ended_utc,
            "started_monotonic_ns": started_ns,
            "ended_monotonic_ns": ended_ns,
            "command": command,
            "command_file_sha256": file_hashes_before,
            "returncode": returncode,
            "launch_error": launch_error,
            "artifact_exists": artifact_exists,
            "artifact_sha256": artifact_sha,
            "artifact_bytes": artifact_bytes,
            "candidate_unchanged": candidate_unchanged,
            "authority_sources_unchanged": sources_unchanged,
        }
        exclusive_write(receipt, canonical_json(receipt_document))
    except (EvidenceInvalid, OSError) as exc:
        print(f"M8 bundle authority runner failed: {exc}", file=sys.stderr)
        return 1

    if returncode != 0 or not artifact_exists or not candidate_unchanged or not sources_unchanged:
        return 2 if returncode != 127 else 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
