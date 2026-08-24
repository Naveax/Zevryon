#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
from typing import Sequence

ROOT = Path(__file__).resolve().parents[1]
SHA_RE = re.compile(r"^[0-9a-fA-F]{40}$")
SCHEMA = "zevryon.m5.physical-candidate.v1"


def parse_args() -> tuple[argparse.Namespace, list[str]]:
    parser = argparse.ArgumentParser(
        description=(
            "Run the M5 physical frame gate only from an exact clean candidate "
            "head and write a source-bound physical admission receipt."
        )
    )
    parser.add_argument("--expected-sha", required=True)
    parser.add_argument(
        "--work-dir",
        type=Path,
        default=ROOT / ".m5-physical-frame",
    )
    parser.add_argument(
        "--receipt",
        type=Path,
        default=None,
        help="Defaults to <work-dir>/physical-candidate-receipt.json",
    )
    args, prepare_args = parser.parse_known_args()
    return args, prepare_args


def _run(command: Sequence[str], *, cwd: Path = ROOT) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        list(command),
        cwd=cwd,
        capture_output=True,
        text=True,
        check=False,
    )


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while True:
            chunk = stream.read(1024 * 1024)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def _validate_prepare_args(prepare_args: Sequence[str]) -> None:
    if "--skip-build" in prepare_args:
        raise ValueError(
            "source-bound physical certification forbids --skip-build; "
            "the measured binaries must be rebuilt from the exact candidate head"
        )


def _git_candidate_state(expected_sha: str) -> dict[str, object]:
    normalized = expected_sha.lower()
    if SHA_RE.fullmatch(normalized) is None:
        raise ValueError("expected SHA must be exactly 40 hexadecimal characters")

    head = _run(["git", "rev-parse", "HEAD"])
    if head.returncode != 0:
        raise RuntimeError(head.stderr.strip() or "git rev-parse HEAD failed")
    actual = head.stdout.strip().lower()
    if actual != normalized:
        raise ValueError(
            f"physical candidate head mismatch: expected {normalized}, got {actual}"
        )

    tracked = _run(["git", "status", "--porcelain", "--untracked-files=no"])
    if tracked.returncode != 0:
        raise RuntimeError(tracked.stderr.strip() or "git status failed")
    tracked_dirty = bool(tracked.stdout.strip())
    if tracked_dirty:
        raise ValueError("physical candidate has tracked worktree changes")

    return {
        "expected_sha": normalized,
        "git_head_sha": actual,
        "exact_head": True,
        "tracked_worktree_clean": True,
    }


def _certified_manifest(work_dir: Path) -> tuple[Path, Path, dict[str, object]]:
    manifest_path = work_dir / "manifest.json"
    evidence_path = work_dir / "frame-certification.json"
    if not manifest_path.is_file() or not evidence_path.is_file():
        raise RuntimeError("physical frame gate did not produce manifest and evidence")

    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
    if not isinstance(manifest, dict) or not isinstance(evidence, dict):
        raise ValueError("physical frame artifacts must be JSON objects")

    checks = evidence.get("checks")
    certified = bool(
        isinstance(checks, dict) and checks.get("native_frame_certified", False)
    )
    if not certified:
        raise ValueError("physical frame evidence is not certified")

    embedded = manifest.get("certification")
    if embedded != evidence:
        raise ValueError("physical manifest certification differs from evidence artifact")

    artifacts = manifest.get("artifacts")
    if not isinstance(artifacts, dict):
        raise ValueError("physical manifest artifact table is missing")
    manifest_evidence_sha = artifacts.get("evidence_sha256")
    actual_evidence_sha = _sha256(evidence_path)
    if manifest_evidence_sha != actual_evidence_sha:
        raise ValueError("physical evidence digest does not match physical manifest")

    return manifest_path, evidence_path, manifest


def build_receipt(
    *,
    candidate: dict[str, object],
    manifest_path: Path,
    evidence_path: Path,
    manifest: dict[str, object],
) -> dict[str, object]:
    return {
        "schema": SCHEMA,
        "source": candidate,
        "checks": {
            "exact_candidate_head": bool(candidate.get("exact_head", False)),
            "tracked_worktree_clean": bool(
                candidate.get("tracked_worktree_clean", False)
            ),
            "native_frame_certified": True,
            "manifest_embeds_exact_evidence": True,
            "candidate_binaries_rebuilt": True,
        },
        "physical": {
            "manifest_sha256": _sha256(manifest_path),
            "evidence_sha256": _sha256(evidence_path),
            "manifest_schema": manifest.get("schema"),
            "certification_schema_version": (
                manifest.get("certification", {}).get("schema_version")
                if isinstance(manifest.get("certification"), dict)
                else None
            ),
        },
    }


def main() -> int:
    args, prepare_args = parse_args()
    try:
        _validate_prepare_args(prepare_args)
        candidate = _git_candidate_state(args.expected_sha)
        work_dir = args.work_dir.resolve()
        receipt_path = (
            args.receipt.resolve()
            if args.receipt is not None
            else work_dir / "physical-candidate-receipt.json"
        )

        prepare = ROOT / "scripts" / "prepare_m5_physical_frame.py"
        command = [
            sys.executable,
            str(prepare),
            "--work-dir",
            str(work_dir),
            *prepare_args,
        ]
        completed = _run(command)
        if completed.returncode != 0:
            if completed.stdout:
                print(completed.stdout.rstrip(), file=sys.stderr)
            if completed.stderr:
                print(completed.stderr.rstrip(), file=sys.stderr)
            return completed.returncode

        manifest_path, evidence_path, manifest = _certified_manifest(work_dir)
        receipt = build_receipt(
            candidate=candidate,
            manifest_path=manifest_path,
            evidence_path=evidence_path,
            manifest=manifest,
        )
        receipt["checks"]["physical_candidate_certified"] = all(
            receipt["checks"].values()
        )
        receipt_path.parent.mkdir(parents=True, exist_ok=True)
        receipt_path.write_text(
            json.dumps(receipt, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        print(
            "physical_candidate_certified=true "
            f"sha={candidate['git_head_sha']} receipt={receipt_path}"
        )
        return 0
    except (OSError, ValueError, RuntimeError, json.JSONDecodeError) as exc:
        print(f"physical candidate certification failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
