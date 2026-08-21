#!/usr/bin/env python3
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import platform as host_platform
import re
import subprocess

SCHEMA = "zevryon.m5.exact-head-ci.v1"
SMOKE_TEST = "python-frame-evidence-contract-smoke"
SHA_RE = re.compile(r"^[0-9a-fA-F]{40}$")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Write exact-head M5 Windows/Linux CI evidence after the full headless CTest suite passes."
    )
    parser.add_argument("--sha", required=True)
    parser.add_argument("--platform", required=True, choices=("linux", "windows"))
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--config")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--ctest", default="ctest")
    return parser.parse_args()


def _run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, check=False, capture_output=True, text=True)


def _git_head() -> tuple[str, str]:
    completed = _run(["git", "rev-parse", "HEAD"])
    if completed.returncode != 0:
        return "", completed.stderr.strip() or "git rev-parse HEAD failed"
    return completed.stdout.strip().lower(), ""


def _run_smoke(args: argparse.Namespace) -> tuple[bool, str]:
    command = [args.ctest, "--test-dir", str(args.build_dir)]
    if args.config:
        command.extend(["-C", args.config])
    command.extend(["--output-on-failure", "-R", f"^{SMOKE_TEST}$"])
    completed = _run(command)
    transcript = "\n".join(
        part for part in (completed.stdout.strip(), completed.stderr.strip()) if part
    )
    passed = completed.returncode == 0 and SMOKE_TEST in transcript
    if completed.returncode == 0 and not transcript:
        passed = False
        transcript = "ctest returned success without output"
    return passed, transcript[-8000:]


def _github_context() -> dict[str, str]:
    names = (
        "GITHUB_RUN_ID",
        "GITHUB_RUN_ATTEMPT",
        "GITHUB_REF",
        "GITHUB_EVENT_NAME",
        "GITHUB_WORKFLOW",
        "GITHUB_JOB",
    )
    return {name.lower(): os.environ.get(name, "")[:512] for name in names}


def _write(path: Path, payload: dict[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def main() -> int:
    args = parse_args()
    expected_sha = args.sha.lower()
    sha_format_valid = SHA_RE.fullmatch(expected_sha) is not None
    git_head_sha, git_error = _git_head()
    exact_head = sha_format_valid and git_head_sha == expected_sha

    smoke_passed = False
    smoke_transcript = ""
    if exact_head:
        smoke_passed, smoke_transcript = _run_smoke(args)

    checks = {
        "expected_sha_format_valid": sha_format_valid,
        "git_head_matches_expected_sha": exact_head,
        "python_evidence_contract_smoke_passed": smoke_passed,
    }
    checks["exact_head_ci_evidence_valid"] = all(checks.values())

    payload: dict[str, object] = {
        "schema": SCHEMA,
        "captured_at_utc": datetime.now(timezone.utc).isoformat().replace("+00:00", "Z"),
        "expected_sha": expected_sha,
        "git_head_sha": git_head_sha,
        "platform": args.platform,
        "compiler": args.compiler,
        "configuration": args.config or "",
        "python": host_platform.python_version(),
        "checks": checks,
        "github": _github_context(),
        "smoke_test": {
            "name": SMOKE_TEST,
            "transcript_tail": smoke_transcript,
        },
    }
    if git_error:
        payload["git_error"] = git_error

    _write(args.output, payload)
    print(
        f"sha={expected_sha} platform={args.platform} "
        f"exact_head={str(exact_head).lower()} "
        f"smoke={str(smoke_passed).lower()} "
        f"evidence_valid={str(checks['exact_head_ci_evidence_valid']).lower()}"
    )
    return 0 if checks["exact_head_ci_evidence_valid"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
