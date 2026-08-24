#!/usr/bin/env python3
from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "scripts" / "certify_m5_physical_candidate.py"
SPEC = importlib.util.spec_from_file_location("certify_m5_physical_candidate", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
module = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(module)


def require(value: bool, message: str) -> None:
    if not value:
        raise RuntimeError(message)


def test_exact_head_and_clean_tree() -> None:
    expected = "a" * 40
    original = module._run

    def fake_run(command, *, cwd=module.ROOT):
        if list(command) == ["git", "rev-parse", "HEAD"]:
            return subprocess.CompletedProcess(command, 0, expected + "\n", "")
        if list(command) == ["git", "status", "--porcelain", "--untracked-files=no"]:
            return subprocess.CompletedProcess(command, 0, "", "")
        raise AssertionError(f"unexpected command: {command}")

    module._run = fake_run
    try:
        state = module._git_candidate_state(expected.upper())
    finally:
        module._run = original
    require(state["git_head_sha"] == expected, "candidate SHA was not normalized")
    require(state["exact_head"] is True, "candidate exact-head check changed")
    require(
        state["tracked_worktree_clean"] is True,
        "candidate clean-tree check changed",
    )


def test_head_mismatch_and_dirty_tree_fail_closed() -> None:
    expected = "b" * 40
    original = module._run

    def mismatch(command, *, cwd=module.ROOT):
        return subprocess.CompletedProcess(command, 0, "c" * 40 + "\n", "")

    module._run = mismatch
    try:
        try:
            module._git_candidate_state(expected)
        except ValueError as exc:
            require("head mismatch" in str(exc), "head mismatch diagnostic changed")
        else:
            raise RuntimeError("mismatched physical candidate head was accepted")
    finally:
        module._run = original

    def dirty(command, *, cwd=module.ROOT):
        if list(command) == ["git", "rev-parse", "HEAD"]:
            return subprocess.CompletedProcess(command, 0, expected + "\n", "")
        return subprocess.CompletedProcess(command, 0, " M src/file.cpp\n", "")

    module._run = dirty
    try:
        try:
            module._git_candidate_state(expected)
        except ValueError as exc:
            require("tracked worktree changes" in str(exc), "dirty-tree diagnostic changed")
        else:
            raise RuntimeError("dirty physical candidate tree was accepted")
    finally:
        module._run = original


def test_manifest_and_evidence_are_digest_bound() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        evidence_path = root / "frame-certification.json"
        evidence = {
            "schema_version": 1,
            "checks": {"native_frame_certified": True},
        }
        evidence_path.write_text(
            json.dumps(evidence, sort_keys=True, separators=(",", ":")),
            encoding="utf-8",
        )
        manifest_path = root / "manifest.json"
        manifest = {
            "schema": "zevryon.m5.physical-frame-run.v1",
            "artifacts": {"evidence_sha256": module._sha256(evidence_path)},
            "certification": evidence,
        }
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")

        parsed_manifest_path, parsed_evidence_path, parsed_manifest = (
            module._certified_manifest(root)
        )
        receipt = module.build_receipt(
            candidate={
                "expected_sha": "d" * 40,
                "git_head_sha": "d" * 40,
                "exact_head": True,
                "tracked_worktree_clean": True,
            },
            manifest_path=parsed_manifest_path,
            evidence_path=parsed_evidence_path,
            manifest=parsed_manifest,
        )
        require(
            receipt["checks"]["native_frame_certified"] is True,
            "receipt lost native certification",
        )
        require(
            receipt["physical"]["evidence_sha256"] == module._sha256(evidence_path),
            "receipt evidence digest changed",
        )

        manifest["artifacts"]["evidence_sha256"] = "0" * 64
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
        try:
            module._certified_manifest(root)
        except ValueError as exc:
            require("digest" in str(exc), "digest mismatch diagnostic changed")
        else:
            raise RuntimeError("physical evidence digest mismatch was accepted")


def main() -> int:
    test_exact_head_and_clean_tree()
    test_head_mismatch_and_dirty_tree_fail_closed()
    test_manifest_and_evidence_are_digest_bound()
    print("Zevryon physical candidate binding smoke passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
