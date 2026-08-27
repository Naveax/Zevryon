#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys
from typing import Mapping

from browser_competitor_registry import CANONICAL_KEYS, get_spec
from m7_admission_replay import resolve_artifact_path
from m7_admission_replay_v3 import (
    AdmissionReplayInvalid,
    INPUT_ARTIFACT_KEYS,
    replay_admission,
    validate_replay_receipt,
)
from m7_collection_admission_v3 import ADMISSION_AUTHORITY, ADMISSION_SCHEMA
from m7_evidence_bundle_manifest import git_source_receipt
from m7_json_artifact_snapshot import (
    JsonArtifactSnapshot,
    JsonArtifactSnapshotInvalid,
    read_json_object_snapshot,
)
from m7_leadership_evaluator import EVALUATOR_SCHEMA
from m7_physical_browser_full_set import PHYSICAL_BROWSER_FULL_SET_AUTHORITY
from m7_physical_host_evidence import PHYSICAL_HOST_AUTHORITY


BUNDLE_SCHEMA = "zevryon.competitor.evidence-bundle-manifest.v3"
BUNDLE_AUTHORITY = "m7-atomic-physical-stage-evidence-publication-v3"
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


def _mapping(value: object, field: str) -> Mapping[str, object]:
    if not isinstance(value, Mapping):
        raise EvidenceBundleInvalid(f"{field} must be an object")
    return value


def _text(value: object, field: str) -> str:
    if not isinstance(value, str) or not value.strip():
        raise EvidenceBundleInvalid(f"{field} must be non-empty text")
    return value.strip()


def _sha256(value: object, field: str) -> str:
    if not isinstance(value, str) or _SHA256_RE.fullmatch(value) is None:
        raise EvidenceBundleInvalid(f"{field} must be a lowercase SHA-256")
    return value


def _git_object(value: object, field: str) -> str:
    if not isinstance(value, str) or _GIT_OBJECT_RE.fullmatch(value) is None:
        raise EvidenceBundleInvalid(f"{field} must be a lowercase 40-hex Git object id")
    return value


def _byte_count(value: object, field: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
        raise EvidenceBundleInvalid(f"{field} must be a positive integer")
    return value


def _physical_receipt(value: object, field: str) -> Mapping[str, object]:
    receipt = _mapping(value, field)
    if receipt.get("authority") != PHYSICAL_HOST_AUTHORITY:
        raise EvidenceBundleInvalid(f"physical host authority drifted: {field}")
    if receipt.get("physical_host_gate_passed") is not True:
        raise EvidenceBundleInvalid(f"physical host gate did not pass: {field}")
    checks = _mapping(receipt.get("checks"), f"{field}.checks")
    if checks.get("physical_metadata_complete") is not True:
        raise EvidenceBundleInvalid(f"physical host checks incomplete: {field}")
    _text(receipt.get("captured_at_utc"), f"{field}.captured_at_utc")
    _text(receipt.get("cpu_model"), f"{field}.cpu_model")
    thermal = _mapping(receipt.get("thermal"), f"{field}.thermal")
    _text(thermal.get("source"), f"{field}.thermal.source")
    return receipt


def _validate_physical_host_evidence(value: object) -> Mapping[str, object]:
    physical = _mapping(value, "physical_host_evidence")
    if physical.get("physical_host_gate_passed") is not True:
        raise EvidenceBundleInvalid("physical host evidence gate did not pass")

    _physical_receipt(physical.get("runtime_preflight"), "runtime_preflight")

    browser = _mapping(physical.get("browser_full_set"), "browser_full_set")
    if browser.get("stage_authority") != PHYSICAL_BROWSER_FULL_SET_AUTHORITY:
        raise EvidenceBundleInvalid("browser full-set physical-stage authority drifted")
    if browser.get("physical_host_gate_passed") is not True:
        raise EvidenceBundleInvalid("browser full-set physical-stage gate did not pass")
    _physical_receipt(browser.get("before"), "browser_full_set.before")
    _physical_receipt(browser.get("after"), "browser_full_set.after")

    for key in ("zevryon_virtualized", "zevryon_native_dom"):
        stage = _mapping(physical.get(key), key)
        if stage.get("physical_host_gate_passed") is not True:
            raise EvidenceBundleInvalid(f"Zevryon physical-stage gate did not pass: {key}")
        _physical_receipt(stage.get("before"), f"{key}.before")
        _physical_receipt(stage.get("after"), f"{key}.after")
    return physical


def _admission_core_payload(admission: Mapping[str, object]) -> dict[str, object]:
    return {
        "admission_authority": admission["admission_authority"],
        "corpus_sha256": admission["corpus_sha256"],
        "leadership_eligible": admission["leadership_eligible"],
        "leadership_evaluation": admission["leadership_evaluation"],
        "leadership_metric_gate_evaluated": admission["leadership_metric_gate_evaluated"],
        "physical_host_evidence": admission["physical_host_evidence"],
        "runtime_bindings": admission["runtime_bindings"],
        "schema": admission["schema"],
        "system_fingerprint": admission["system_fingerprint"],
    }


def validate_admission_for_publication(admission: Mapping[str, object]) -> None:
    if admission.get("schema") != ADMISSION_SCHEMA:
        raise EvidenceBundleInvalid("collection admission v3 schema mismatch")
    if admission.get("admission_authority") != ADMISSION_AUTHORITY:
        raise EvidenceBundleInvalid("collection admission v3 authority mismatch")
    _sha256(admission.get("system_fingerprint"), "system_fingerprint")
    _sha256(admission.get("corpus_sha256"), "corpus_sha256")
    _validate_physical_host_evidence(admission.get("physical_host_evidence"))

    if admission.get("leadership_metric_gate_evaluated") is not True:
        raise EvidenceBundleInvalid("collection admission did not evaluate the metric gate")
    if not isinstance(admission.get("leadership_eligible"), bool):
        raise EvidenceBundleInvalid("leadership_eligible must be boolean")

    bindings = _mapping(admission.get("runtime_bindings"), "runtime_bindings")
    if set(bindings) != set(CANONICAL_KEYS):
        raise EvidenceBundleInvalid("runtime binding set drifted")
    for competitor in CANONICAL_KEYS:
        binding = _mapping(bindings.get(competitor), f"runtime_bindings.{competitor}")
        if binding.get("adapter") != get_spec(competitor).adapter:
            raise EvidenceBundleInvalid(f"runtime binding adapter drifted: {competitor}")
        if binding.get("matched") is not True:
            raise EvidenceBundleInvalid(f"runtime binding did not match: {competitor}")
        _text(
            binding.get("stable_runtime_identity"),
            f"runtime_bindings.{competitor}.stable_runtime_identity",
        )

    evaluation = _mapping(admission.get("leadership_evaluation"), "leadership_evaluation")
    if evaluation.get("schema") != EVALUATOR_SCHEMA:
        raise EvidenceBundleInvalid("leadership evaluation schema mismatch")
    if evaluation.get("leadership_metric_gate_evaluated") is not True:
        raise EvidenceBundleInvalid("leadership evaluation metric gate was not evaluated")
    if evaluation.get("leadership_eligible") is not admission.get("leadership_eligible"):
        raise EvidenceBundleInvalid("admission/evaluator leadership eligibility drifted")

    artifacts = _mapping(admission.get("input_artifacts"), "input_artifacts")
    if set(artifacts) != set(INPUT_ARTIFACT_KEYS):
        raise EvidenceBundleInvalid("input artifact set drifted")
    for name in INPUT_ARTIFACT_KEYS:
        receipt = _mapping(artifacts.get(name), f"input_artifacts.{name}")
        _text(receipt.get("path"), f"input_artifacts.{name}.path")
        _sha256(receipt.get("sha256"), f"input_artifacts.{name}.sha256")
        _byte_count(receipt.get("byte_count"), f"input_artifacts.{name}.byte_count")


def read_admission_snapshot(
    admission_path: Path,
    *,
    artifact_root: Path,
) -> JsonArtifactSnapshot:
    try:
        resolved = resolve_artifact_path(artifact_root, admission_path)
        snapshot = read_json_object_snapshot(resolved, label="collection admission v3")
    except (ValueError, JsonArtifactSnapshotInvalid) as exc:
        raise EvidenceBundleInvalid(f"collection admission artifact invalid: {exc}") from exc
    validate_admission_for_publication(snapshot.value)
    return snapshot


def _validate_replay_against_admission(
    replay_value: object,
    admission: Mapping[str, object],
) -> Mapping[str, object]:
    try:
        replay = validate_replay_receipt(replay_value)
    except AdmissionReplayInvalid as exc:
        raise EvidenceBundleInvalid(f"admission replay receipt invalid: {exc}") from exc

    expected_admission_sha = canonical_sha256(_admission_core_payload(admission))
    if replay.get("recomputed_admission_sha256") != expected_admission_sha:
        raise EvidenceBundleInvalid(
            "admission replay does not bind the stored v3 admission core payload"
        )

    artifacts = _mapping(admission.get("input_artifacts"), "input_artifacts")
    hashes = _mapping(replay.get("input_artifact_sha256"), "replay hashes")
    counts = _mapping(replay.get("input_artifact_byte_count"), "replay byte counts")
    for name in INPUT_ARTIFACT_KEYS:
        receipt = _mapping(artifacts.get(name), f"input_artifacts.{name}")
        if hashes.get(name) != receipt.get("sha256"):
            raise EvidenceBundleInvalid(f"replay/admission artifact SHA drifted: {name}")
        if counts.get(name) != receipt.get("byte_count"):
            raise EvidenceBundleInvalid(f"replay/admission artifact byte count drifted: {name}")
    return replay


def build_bundle_manifest(
    admission_snapshot: JsonArtifactSnapshot,
    *,
    admission_replay: Mapping[str, object],
    source: Mapping[str, object],
) -> dict[str, object]:
    admission = admission_snapshot.value
    validate_admission_for_publication(admission)
    replay = _validate_replay_against_admission(admission_replay, admission)

    commit = _git_object(source.get("commit"), "source.commit")
    tree = _git_object(source.get("tree"), "source.tree")
    if source.get("tracked_worktree_clean") is not True:
        raise EvidenceBundleInvalid("source receipt does not prove a clean tracked worktree")

    eligible = admission.get("leadership_eligible") is True
    admission_core_sha = canonical_sha256(_admission_core_payload(admission))
    payload: dict[str, object] = {
        "schema": BUNDLE_SCHEMA,
        "bundle_authority": BUNDLE_AUTHORITY,
        "source": {**dict(source), "commit": commit, "tree": tree},
        "admission_contract": {
            "schema": ADMISSION_SCHEMA,
            "authority": ADMISSION_AUTHORITY,
            "recomputed_admission_sha256": admission_core_sha,
        },
        "admission_artifact": {
            "path": str(admission_snapshot.path),
            "sha256": admission_snapshot.sha256,
            "byte_count": admission_snapshot.byte_count,
        },
        "raw_artifacts": {
            name: dict(_mapping(admission["input_artifacts"][name], f"input_artifacts.{name}"))
            for name in INPUT_ARTIFACT_KEYS
        },
        "admission_replay": dict(replay),
        "system_fingerprint": admission["system_fingerprint"],
        "corpus_sha256": admission["corpus_sha256"],
        "physical_host_evidence": admission["physical_host_evidence"],
        "runtime_bindings": admission["runtime_bindings"],
        "leadership_evaluation": admission["leadership_evaluation"],
        "leadership_metric_gate_evaluated": True,
        "leadership_eligible": eligible,
        "result_class": "leadership_eligible" if eligible else "valid_not_leadership",
    }
    manifest = {**payload, "manifest_payload_sha256": canonical_sha256(payload)}
    validate_bundle_manifest(manifest)
    return manifest


def validate_bundle_manifest(manifest: Mapping[str, object]) -> None:
    if manifest.get("schema") != BUNDLE_SCHEMA:
        raise EvidenceBundleInvalid("bundle manifest v3 schema mismatch")
    if manifest.get("bundle_authority") != BUNDLE_AUTHORITY:
        raise EvidenceBundleInvalid("bundle manifest v3 authority mismatch")
    _sha256(manifest.get("system_fingerprint"), "system_fingerprint")
    _sha256(manifest.get("corpus_sha256"), "corpus_sha256")
    _validate_physical_host_evidence(manifest.get("physical_host_evidence"))

    if manifest.get("leadership_metric_gate_evaluated") is not True:
        raise EvidenceBundleInvalid("bundle metric gate was not evaluated")
    if not isinstance(manifest.get("leadership_eligible"), bool):
        raise EvidenceBundleInvalid("bundle leadership_eligible must be boolean")
    expected_class = (
        "leadership_eligible"
        if manifest.get("leadership_eligible") is True
        else "valid_not_leadership"
    )
    if manifest.get("result_class") != expected_class:
        raise EvidenceBundleInvalid("bundle result class drifted")

    source = _mapping(manifest.get("source"), "source")
    _git_object(source.get("commit"), "source.commit")
    _git_object(source.get("tree"), "source.tree")
    if source.get("tracked_worktree_clean") is not True:
        raise EvidenceBundleInvalid("bundle source receipt is not clean")

    contract = _mapping(manifest.get("admission_contract"), "admission_contract")
    if contract.get("schema") != ADMISSION_SCHEMA:
        raise EvidenceBundleInvalid("bundle admission contract schema drifted")
    if contract.get("authority") != ADMISSION_AUTHORITY:
        raise EvidenceBundleInvalid("bundle admission contract authority drifted")
    contract_sha = _sha256(
        contract.get("recomputed_admission_sha256"),
        "admission_contract.recomputed_admission_sha256",
    )

    admission = _mapping(manifest.get("admission_artifact"), "admission_artifact")
    _text(admission.get("path"), "admission_artifact.path")
    _sha256(admission.get("sha256"), "admission_artifact.sha256")
    _byte_count(admission.get("byte_count"), "admission_artifact.byte_count")

    raw = _mapping(manifest.get("raw_artifacts"), "raw_artifacts")
    if set(raw) != set(INPUT_ARTIFACT_KEYS):
        raise EvidenceBundleInvalid("bundle raw artifact set drifted")
    for name in INPUT_ARTIFACT_KEYS:
        receipt = _mapping(raw.get(name), f"raw_artifacts.{name}")
        _text(receipt.get("path"), f"raw_artifacts.{name}.path")
        _sha256(receipt.get("sha256"), f"raw_artifacts.{name}.sha256")
        _byte_count(receipt.get("byte_count"), f"raw_artifacts.{name}.byte_count")

    replay = _mapping(manifest.get("admission_replay"), "admission_replay")
    try:
        validate_replay_receipt(replay)
    except AdmissionReplayInvalid as exc:
        raise EvidenceBundleInvalid(f"bundle replay receipt invalid: {exc}") from exc
    if replay.get("recomputed_admission_sha256") != contract_sha:
        raise EvidenceBundleInvalid("bundle replay/admission-contract SHA drifted")
    hashes = _mapping(replay.get("input_artifact_sha256"), "replay hashes")
    counts = _mapping(replay.get("input_artifact_byte_count"), "replay byte counts")
    for name in INPUT_ARTIFACT_KEYS:
        if hashes.get(name) != raw[name].get("sha256"):
            raise EvidenceBundleInvalid(f"bundle replay/raw SHA drifted: {name}")
        if counts.get(name) != raw[name].get("byte_count"):
            raise EvidenceBundleInvalid(f"bundle replay/raw byte count drifted: {name}")

    bindings = _mapping(manifest.get("runtime_bindings"), "runtime_bindings")
    if set(bindings) != set(CANONICAL_KEYS):
        raise EvidenceBundleInvalid("bundle runtime binding set drifted")
    evaluation = _mapping(manifest.get("leadership_evaluation"), "leadership_evaluation")
    if evaluation.get("schema") != EVALUATOR_SCHEMA:
        raise EvidenceBundleInvalid("bundle leadership evaluation schema mismatch")
    if evaluation.get("leadership_eligible") is not manifest.get("leadership_eligible"):
        raise EvidenceBundleInvalid("bundle/evaluator eligibility drifted")

    reconstructed_admission_core = {
        "admission_authority": contract["authority"],
        "corpus_sha256": manifest["corpus_sha256"],
        "leadership_eligible": manifest["leadership_eligible"],
        "leadership_evaluation": manifest["leadership_evaluation"],
        "leadership_metric_gate_evaluated": manifest["leadership_metric_gate_evaluated"],
        "physical_host_evidence": manifest["physical_host_evidence"],
        "runtime_bindings": manifest["runtime_bindings"],
        "schema": contract["schema"],
        "system_fingerprint": manifest["system_fingerprint"],
    }
    if canonical_sha256(reconstructed_admission_core) != contract_sha:
        raise EvidenceBundleInvalid("bundle fields do not reproduce the bound v3 admission core")

    recorded = _sha256(manifest.get("manifest_payload_sha256"), "manifest_payload_sha256")
    payload = dict(manifest)
    payload.pop("manifest_payload_sha256", None)
    if recorded != canonical_sha256(payload):
        raise EvidenceBundleInvalid("bundle manifest payload SHA drifted")


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Publish one M7 v3 evidence bundle after atomic admission/raw-artifact replay "
            "and exact clean Git commit/tree verification."
        )
    )
    parser.add_argument("--admission", type=Path, required=True)
    parser.add_argument("--artifact-root", type=Path, default=Path("."))
    parser.add_argument("--repo-root", type=Path, default=Path("."))
    parser.add_argument("--expected-tool-commit", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    try:
        admission_snapshot = read_admission_snapshot(
            args.admission,
            artifact_root=args.artifact_root,
        )
        replay = replay_admission(
            admission_snapshot.value,
            artifact_root=args.artifact_root,
        )
        source = git_source_receipt(
            args.repo_root,
            expected_commit=args.expected_tool_commit,
        )
        manifest = build_bundle_manifest(
            admission_snapshot,
            admission_replay=replay,
            source=source,
        )
    except (
        AdmissionReplayInvalid,
        EvidenceBundleInvalid,
        ValueError,
    ) as exc:
        print(f"M7 evidence bundle manifest v3 rejected: {exc}", file=sys.stderr)
        return 1

    text = json.dumps(manifest, indent=2, sort_keys=True) + "\n"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(text, encoding="utf-8")
    print(text, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
