#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
from typing import Callable, Mapping, Sequence

from browser_competitor_registry import CANONICAL_KEYS, get_spec
from m7_admission_replay import (
    AdmissionReplayInvalid,
    replay_admission,
    resolve_artifact_path,
    validate_replay_receipt,
)
from m7_collection_admission import ADMISSION_AUTHORITY, ADMISSION_SCHEMA
from m7_leadership_evaluator import EVALUATOR_SCHEMA
from m7_physical_host_evidence import PHYSICAL_HOST_AUTHORITY


BUNDLE_SCHEMA = "zevryon.competitor.evidence-bundle-manifest.v1"
BUNDLE_AUTHORITY = "m7-canonical-admitted-evidence-publication-v1"
INPUT_ARTIFACT_KEYS = (
    "preflight",
    "browser_report",
    "zevryon_virtualized",
    "zevryon_native_dom",
)
_SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
_GIT_OBJECT_RE = re.compile(r"^[0-9a-f]{40}$")


class EvidenceBundleInvalid(ValueError):
    pass


def canonical_sha256(value: object) -> str:
    encoded = json.dumps(
        value,
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=False,
    ).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as handle:
            while True:
                chunk = handle.read(1024 * 1024)
                if not chunk:
                    break
                digest.update(chunk)
    except OSError as exc:
        raise EvidenceBundleInvalid(f"cannot hash evidence artifact {path}: {exc}") from exc
    return digest.hexdigest()


def _required_sha256(value: object, field: str) -> str:
    if not isinstance(value, str) or _SHA256_RE.fullmatch(value) is None:
        raise EvidenceBundleInvalid(f"{field} must be a lowercase SHA-256")
    return value


def _required_git_object(value: object, field: str) -> str:
    if not isinstance(value, str) or _GIT_OBJECT_RE.fullmatch(value) is None:
        raise EvidenceBundleInvalid(f"{field} must be a lowercase 40-hex Git object id")
    return value


def _required_text(value: object, field: str) -> str:
    if not isinstance(value, str) or not value.strip():
        raise EvidenceBundleInvalid(f"{field} must be non-empty text")
    return value.strip()


def _validate_single_physical_receipt(value: object, field: str) -> Mapping[str, object]:
    if not isinstance(value, Mapping):
        raise EvidenceBundleInvalid(f"physical host receipt is missing: {field}")
    if value.get("authority") != PHYSICAL_HOST_AUTHORITY:
        raise EvidenceBundleInvalid(f"physical host authority drifted: {field}")
    if value.get("physical_host_gate_passed") is not True:
        raise EvidenceBundleInvalid(f"physical host stage did not pass: {field}")
    checks = value.get("checks")
    if not isinstance(checks, Mapping) or checks.get("physical_metadata_complete") is not True:
        raise EvidenceBundleInvalid(f"physical metadata checks are incomplete: {field}")
    _required_text(value.get("captured_at_utc"), f"{field}.captured_at_utc")
    _required_text(value.get("cpu_model"), f"{field}.cpu_model")
    thermal = value.get("thermal")
    if not isinstance(thermal, Mapping):
        raise EvidenceBundleInvalid(f"physical thermal receipt is missing: {field}")
    _required_text(thermal.get("source"), f"{field}.thermal.source")
    return value


def _validate_physical_host_evidence(value: object) -> Mapping[str, object]:
    if not isinstance(value, Mapping):
        raise EvidenceBundleInvalid("collection admission physical host evidence is missing")
    if value.get("physical_host_gate_passed") is not True:
        raise EvidenceBundleInvalid("collection admission physical host gate did not pass")

    _validate_single_physical_receipt(
        value.get("runtime_preflight"),
        "physical_host_evidence.runtime_preflight",
    )
    _validate_single_physical_receipt(
        value.get("browser_full_set"),
        "physical_host_evidence.browser_full_set",
    )
    for mode_key in ("zevryon_virtualized", "zevryon_native_dom"):
        stage = value.get(mode_key)
        if not isinstance(stage, Mapping):
            raise EvidenceBundleInvalid(f"physical Zevryon stage is missing: {mode_key}")
        if stage.get("physical_host_gate_passed") is not True:
            raise EvidenceBundleInvalid(f"physical Zevryon stage did not pass: {mode_key}")
        _validate_single_physical_receipt(
            stage.get("before"),
            f"physical_host_evidence.{mode_key}.before",
        )
        _validate_single_physical_receipt(
            stage.get("after"),
            f"physical_host_evidence.{mode_key}.after",
        )
    return value


def validate_admission_for_publication(admission: Mapping[str, object]) -> None:
    if admission.get("schema") != ADMISSION_SCHEMA:
        raise EvidenceBundleInvalid("collection admission schema mismatch")
    if admission.get("admission_authority") != ADMISSION_AUTHORITY:
        raise EvidenceBundleInvalid("collection admission authority mismatch")
    _required_sha256(admission.get("system_fingerprint"), "system_fingerprint")
    _required_sha256(admission.get("corpus_sha256"), "corpus_sha256")
    _validate_physical_host_evidence(admission.get("physical_host_evidence"))
    if admission.get("leadership_metric_gate_evaluated") is not True:
        raise EvidenceBundleInvalid("collection admission did not evaluate the metric gate")
    if not isinstance(admission.get("leadership_eligible"), bool):
        raise EvidenceBundleInvalid("collection admission leadership_eligible must be boolean")

    bindings = admission.get("runtime_bindings")
    if not isinstance(bindings, Mapping) or set(bindings) != set(CANONICAL_KEYS):
        raise EvidenceBundleInvalid("collection admission runtime binding set drifted")
    for competitor in CANONICAL_KEYS:
        binding = bindings.get(competitor)
        if not isinstance(binding, Mapping):
            raise EvidenceBundleInvalid(f"runtime binding is not an object: {competitor}")
        if binding.get("adapter") != get_spec(competitor).adapter:
            raise EvidenceBundleInvalid(f"runtime binding adapter drifted: {competitor}")
        _required_text(
            binding.get("stable_runtime_identity"),
            f"{competitor}.stable_runtime_identity",
        )
        if binding.get("matched") is not True:
            raise EvidenceBundleInvalid(f"runtime binding did not match: {competitor}")

    evaluation = admission.get("leadership_evaluation")
    if not isinstance(evaluation, Mapping):
        raise EvidenceBundleInvalid("collection admission lacks leadership evaluation")
    if evaluation.get("schema") != EVALUATOR_SCHEMA:
        raise EvidenceBundleInvalid("leadership evaluation schema mismatch")
    if evaluation.get("leadership_metric_gate_evaluated") is not True:
        raise EvidenceBundleInvalid("leadership evaluation metric gate was not evaluated")
    if not isinstance(evaluation.get("leadership_eligible"), bool):
        raise EvidenceBundleInvalid("leadership evaluation eligibility must be boolean")
    if evaluation.get("leadership_eligible") is not admission.get("leadership_eligible"):
        raise EvidenceBundleInvalid("admission/evaluator leadership eligibility drifted")

    artifacts = admission.get("input_artifacts")
    if not isinstance(artifacts, Mapping) or set(artifacts) != set(INPUT_ARTIFACT_KEYS):
        raise EvidenceBundleInvalid("collection admission input artifact set drifted")
    for name in INPUT_ARTIFACT_KEYS:
        receipt = artifacts.get(name)
        if not isinstance(receipt, Mapping):
            raise EvidenceBundleInvalid(f"input artifact receipt is not an object: {name}")
        _required_text(receipt.get("path"), f"input_artifacts.{name}.path")
        _required_sha256(receipt.get("sha256"), f"input_artifacts.{name}.sha256")


def verify_admission_input_artifacts(
    admission: Mapping[str, object],
    *,
    artifact_root: Path,
) -> dict[str, dict[str, str]]:
    validate_admission_for_publication(admission)
    raw_artifacts = admission["input_artifacts"]
    assert isinstance(raw_artifacts, Mapping)
    receipts: dict[str, dict[str, str]] = {}
    for name in INPUT_ARTIFACT_KEYS:
        raw_receipt = raw_artifacts[name]
        assert isinstance(raw_receipt, Mapping)
        declared_path = Path(str(raw_receipt["path"]))
        try:
            resolved = resolve_artifact_path(artifact_root, declared_path)
        except AdmissionReplayInvalid as exc:
            raise EvidenceBundleInvalid(f"input artifact path invalid: {name}: {exc}") from exc
        expected_sha = str(raw_receipt["sha256"])
        actual_sha = file_sha256(resolved)
        if actual_sha != expected_sha:
            raise EvidenceBundleInvalid(
                f"input artifact SHA drifted: {name}; expected={expected_sha}, actual={actual_sha}"
            )
        receipts[name] = {
            "declared_path": str(declared_path),
            "resolved_path": str(resolved),
            "sha256": actual_sha,
        }
    return receipts


GitRunner = Callable[[Sequence[str]], tuple[int, str, str]]


def _default_git_runner(command: Sequence[str]) -> tuple[int, str, str]:
    completed = subprocess.run(
        list(command),
        capture_output=True,
        text=True,
        check=False,
    )
    return int(completed.returncode), completed.stdout, completed.stderr


def git_source_receipt(
    repo_root: Path,
    *,
    expected_commit: str | None = None,
    runner: GitRunner = _default_git_runner,
) -> dict[str, object]:
    root = repo_root.resolve()

    def run(*args: str) -> tuple[int, str, str]:
        return runner(("git", "-C", str(root), *args))

    code, stdout, stderr = run("rev-parse", "HEAD")
    if code != 0:
        detail = (stderr or stdout).strip()
        raise EvidenceBundleInvalid(
            "cannot resolve Git HEAD" + (f": {detail}" if detail else "")
        )
    commit = _required_git_object(stdout.strip().lower(), "git.commit")

    if expected_commit is not None:
        if not isinstance(expected_commit, str):
            raise EvidenceBundleInvalid("expected_tool_commit must be text")
        expected = _required_git_object(
            expected_commit.strip().lower(),
            "expected_tool_commit",
        )
        if commit != expected:
            raise EvidenceBundleInvalid(
                f"Git HEAD differs from admitted tool commit: expected={expected}, actual={commit}"
            )

    code, stdout, stderr = run("rev-parse", "HEAD^{tree}")
    if code != 0:
        detail = (stderr or stdout).strip()
        raise EvidenceBundleInvalid(
            "cannot resolve Git tree" + (f": {detail}" if detail else "")
        )
    tree = _required_git_object(stdout.strip().lower(), "git.tree")

    code, _, stderr = run("diff", "--quiet", "HEAD", "--")
    if code not in {0, 1}:
        raise EvidenceBundleInvalid(
            "cannot inspect tracked Git worktree" + (f": {stderr.strip()}" if stderr.strip() else "")
        )
    if code == 1:
        raise EvidenceBundleInvalid(
            "tracked source files differ from Git HEAD; canonical evidence requires a clean tracked worktree"
        )

    return {
        "repository_root": str(root),
        "commit": commit,
        "tree": tree,
        "tracked_worktree_clean": True,
        "untracked_files_ignored_for_cleanliness": True,
    }


def _validate_replay_against_artifacts(
    replay_value: object,
    artifact_receipts: Mapping[str, Mapping[str, str]],
) -> Mapping[str, object]:
    try:
        replay = validate_replay_receipt(replay_value)
    except AdmissionReplayInvalid as exc:
        raise EvidenceBundleInvalid(f"admission replay receipt invalid: {exc}") from exc
    hashes = replay.get("input_artifact_sha256")
    assert isinstance(hashes, Mapping)
    for name in INPUT_ARTIFACT_KEYS:
        receipt = artifact_receipts.get(name)
        if not isinstance(receipt, Mapping):
            raise EvidenceBundleInvalid(f"verified artifact receipt is missing: {name}")
        if hashes.get(name) != receipt.get("sha256"):
            raise EvidenceBundleInvalid(f"admission replay/raw artifact SHA drifted: {name}")
    return replay


def build_bundle_manifest(
    admission: Mapping[str, object],
    *,
    admission_path: Path,
    admission_sha256: str,
    artifact_receipts: Mapping[str, Mapping[str, str]],
    admission_replay: Mapping[str, object],
    source: Mapping[str, object],
) -> dict[str, object]:
    validate_admission_for_publication(admission)
    admission_digest = _required_sha256(admission_sha256, "admission_sha256")
    if set(artifact_receipts) != set(INPUT_ARTIFACT_KEYS):
        raise EvidenceBundleInvalid("verified artifact receipt set drifted")
    replay_receipt = _validate_replay_against_artifacts(admission_replay, artifact_receipts)
    commit = _required_git_object(source.get("commit"), "source.commit")
    tree = _required_git_object(source.get("tree"), "source.tree")
    if source.get("tracked_worktree_clean") is not True:
        raise EvidenceBundleInvalid("source receipt does not prove a clean tracked worktree")

    eligible = admission["leadership_eligible"] is True
    physical_host_evidence = _validate_physical_host_evidence(
        admission.get("physical_host_evidence")
    )
    payload: dict[str, object] = {
        "schema": BUNDLE_SCHEMA,
        "bundle_authority": BUNDLE_AUTHORITY,
        "source": {
            **dict(source),
            "commit": commit,
            "tree": tree,
        },
        "admission_artifact": {
            "path": str(admission_path),
            "sha256": admission_digest,
        },
        "input_artifacts": {
            name: dict(artifact_receipts[name]) for name in INPUT_ARTIFACT_KEYS
        },
        "admission_replay": dict(replay_receipt),
        "system_fingerprint": admission["system_fingerprint"],
        "corpus_sha256": admission["corpus_sha256"],
        "physical_host_evidence": dict(physical_host_evidence),
        "runtime_bindings": admission["runtime_bindings"],
        "leadership_evaluation": admission["leadership_evaluation"],
        "leadership_metric_gate_evaluated": True,
        "leadership_eligible": eligible,
        "result_class": "leadership_eligible" if eligible else "valid_not_leadership",
    }
    payload_sha = canonical_sha256(payload)
    manifest = {
        **payload,
        "manifest_payload_sha256": payload_sha,
    }
    validate_bundle_manifest(manifest)
    return manifest


def validate_bundle_manifest(manifest: Mapping[str, object]) -> None:
    if manifest.get("schema") != BUNDLE_SCHEMA:
        raise EvidenceBundleInvalid("bundle manifest schema mismatch")
    if manifest.get("bundle_authority") != BUNDLE_AUTHORITY:
        raise EvidenceBundleInvalid("bundle manifest authority mismatch")
    _required_sha256(manifest.get("system_fingerprint"), "system_fingerprint")
    _required_sha256(manifest.get("corpus_sha256"), "corpus_sha256")
    _validate_physical_host_evidence(manifest.get("physical_host_evidence"))
    if manifest.get("leadership_metric_gate_evaluated") is not True:
        raise EvidenceBundleInvalid("bundle manifest metric gate was not evaluated")
    if not isinstance(manifest.get("leadership_eligible"), bool):
        raise EvidenceBundleInvalid("bundle leadership_eligible must be boolean")
    expected_class = (
        "leadership_eligible"
        if manifest.get("leadership_eligible") is True
        else "valid_not_leadership"
    )
    if manifest.get("result_class") != expected_class:
        raise EvidenceBundleInvalid("bundle result class drifted")

    source = manifest.get("source")
    if not isinstance(source, Mapping):
        raise EvidenceBundleInvalid("bundle source receipt is missing")
    _required_git_object(source.get("commit"), "source.commit")
    _required_git_object(source.get("tree"), "source.tree")
    if source.get("tracked_worktree_clean") is not True:
        raise EvidenceBundleInvalid("bundle source receipt is not clean")

    admission = manifest.get("admission_artifact")
    if not isinstance(admission, Mapping):
        raise EvidenceBundleInvalid("bundle admission artifact receipt is missing")
    _required_text(admission.get("path"), "admission_artifact.path")
    _required_sha256(admission.get("sha256"), "admission_artifact.sha256")

    artifacts = manifest.get("input_artifacts")
    if not isinstance(artifacts, Mapping) or set(artifacts) != set(INPUT_ARTIFACT_KEYS):
        raise EvidenceBundleInvalid("bundle input artifact set drifted")
    for name in INPUT_ARTIFACT_KEYS:
        receipt = artifacts.get(name)
        if not isinstance(receipt, Mapping):
            raise EvidenceBundleInvalid(f"bundle artifact receipt is invalid: {name}")
        _required_text(receipt.get("declared_path"), f"{name}.declared_path")
        _required_text(receipt.get("resolved_path"), f"{name}.resolved_path")
        _required_sha256(receipt.get("sha256"), f"{name}.sha256")

    replay = _validate_replay_against_artifacts(manifest.get("admission_replay"), artifacts)
    if replay.get("replay_gate_passed") is not True:
        raise EvidenceBundleInvalid("bundle admission replay did not pass")

    evaluation = manifest.get("leadership_evaluation")
    if not isinstance(evaluation, Mapping) or evaluation.get("schema") != EVALUATOR_SCHEMA:
        raise EvidenceBundleInvalid("bundle leadership evaluation is invalid")
    if evaluation.get("leadership_eligible") is not manifest.get("leadership_eligible"):
        raise EvidenceBundleInvalid("bundle/evaluator leadership eligibility drifted")

    bindings = manifest.get("runtime_bindings")
    if not isinstance(bindings, Mapping) or set(bindings) != set(CANONICAL_KEYS):
        raise EvidenceBundleInvalid("bundle runtime binding set drifted")

    recorded_payload_sha = _required_sha256(
        manifest.get("manifest_payload_sha256"),
        "manifest_payload_sha256",
    )
    payload = dict(manifest)
    payload.pop("manifest_payload_sha256", None)
    expected_payload_sha = canonical_sha256(payload)
    if recorded_payload_sha != expected_payload_sha:
        raise EvidenceBundleInvalid("bundle manifest payload SHA drifted")


def _read_object(path: Path, label: str) -> Mapping[str, object]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise EvidenceBundleInvalid(f"cannot read {label}: {exc}") from exc
    if not isinstance(value, Mapping):
        raise EvidenceBundleInvalid(f"{label} must be a JSON object")
    return value


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Create a canonical publication manifest for one physically certified admitted M7 evidence bundle. "
            "The manifest constrains raw artifacts to artifact_root, verifies their SHA receipts, replays collection admission, and binds the exact clean Git commit/tree."
        )
    )
    parser.add_argument("--admission", type=Path, required=True)
    parser.add_argument("--artifact-root", type=Path, default=Path("."))
    parser.add_argument("--repo-root", type=Path, default=Path("."))
    parser.add_argument("--expected-tool-commit", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    try:
        admission = _read_object(args.admission, "collection admission")
        admission_sha = file_sha256(args.admission)
        artifacts = verify_admission_input_artifacts(
            admission,
            artifact_root=args.artifact_root,
        )
        replay = replay_admission(
            admission,
            artifact_root=args.artifact_root,
        )
        source = git_source_receipt(
            args.repo_root,
            expected_commit=args.expected_tool_commit,
        )
        manifest = build_bundle_manifest(
            admission,
            admission_path=args.admission,
            admission_sha256=admission_sha,
            artifact_receipts=artifacts,
            admission_replay=replay,
            source=source,
        )
    except (EvidenceBundleInvalid, AdmissionReplayInvalid) as exc:
        print(f"M7 evidence bundle manifest rejected: {exc}", file=sys.stderr)
        return 1

    text = json.dumps(manifest, indent=2, sort_keys=True) + "\n"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(text, encoding="utf-8")
    print(text, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
