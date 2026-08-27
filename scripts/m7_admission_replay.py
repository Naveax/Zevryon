#!/usr/bin/env python3
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re
from typing import Mapping

from m7_collection_admission import CollectionAdmissionInvalid, admit_collection


REPLAY_SCHEMA = "zevryon.competitor.collection-admission-replay.v1"
REPLAY_AUTHORITY = "m7-raw-artifacts-recompute-admission-v1"
INPUT_ARTIFACT_KEYS = (
    "preflight",
    "browser_report",
    "zevryon_virtualized",
    "zevryon_native_dom",
)
_SHA256_RE = re.compile(r"^[0-9a-f]{64}$")


class AdmissionReplayInvalid(ValueError):
    pass


def _mapping(value: object, field: str) -> Mapping[str, object]:
    if not isinstance(value, Mapping):
        raise AdmissionReplayInvalid(f"{field} must be an object")
    return value


def _text(value: object, field: str) -> str:
    if not isinstance(value, str) or not value.strip():
        raise AdmissionReplayInvalid(f"{field} must be non-empty text")
    return value.strip()


def _sha256(value: object, field: str) -> str:
    if not isinstance(value, str) or _SHA256_RE.fullmatch(value) is None:
        raise AdmissionReplayInvalid(f"{field} must be a lowercase SHA-256")
    return value


def resolve_artifact_path(artifact_root: Path, declared_path: Path) -> Path:
    root = artifact_root.resolve()
    candidate = declared_path if declared_path.is_absolute() else root / declared_path
    try:
        resolved = candidate.resolve()
    except OSError as exc:
        raise AdmissionReplayInvalid(
            f"cannot resolve raw artifact path {declared_path}: {exc}"
        ) from exc
    try:
        resolved.relative_to(root)
    except ValueError as exc:
        raise AdmissionReplayInvalid(
            f"raw artifact path escapes artifact_root: {declared_path}"
        ) from exc
    if resolved == root:
        raise AdmissionReplayInvalid("raw artifact path resolves to artifact_root itself")
    return resolved


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
        raise AdmissionReplayInvalid(f"cannot hash raw artifact {path}: {exc}") from exc
    return digest.hexdigest()


def _read_object(path: Path, label: str) -> Mapping[str, object]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise AdmissionReplayInvalid(f"cannot read {label}: {exc}") from exc
    if not isinstance(value, Mapping):
        raise AdmissionReplayInvalid(f"{label} must be a JSON object")
    return value


def replay_admission(
    admission: Mapping[str, object],
    *,
    artifact_root: Path,
) -> dict[str, object]:
    receipts = _mapping(admission.get("input_artifacts"), "input_artifacts")
    if set(receipts) != set(INPUT_ARTIFACT_KEYS):
        raise AdmissionReplayInvalid("admission raw artifact set drifted")

    raw: dict[str, Mapping[str, object]] = {}
    verified_sha: dict[str, str] = {}
    for name in INPUT_ARTIFACT_KEYS:
        receipt = _mapping(receipts.get(name), f"input_artifacts.{name}")
        declared = Path(_text(receipt.get("path"), f"input_artifacts.{name}.path"))
        expected = _sha256(receipt.get("sha256"), f"input_artifacts.{name}.sha256")
        resolved = resolve_artifact_path(artifact_root, declared)
        actual = file_sha256(resolved)
        if actual != expected:
            raise AdmissionReplayInvalid(
                f"raw artifact SHA drifted: {name}; expected={expected}, actual={actual}"
            )
        raw[name] = _read_object(resolved, name)
        verified_sha[name] = actual

    try:
        recomputed = admit_collection(
            raw["preflight"],
            raw["browser_report"],
            raw["zevryon_virtualized"],
            raw["zevryon_native_dom"],
        )
    except CollectionAdmissionInvalid as exc:
        raise AdmissionReplayInvalid(
            f"raw artifacts no longer reproduce a valid collection admission: {exc}"
        ) from exc

    expected_keys = set(recomputed) | {"input_artifacts"}
    if set(admission) != expected_keys:
        extra = sorted(set(admission) - expected_keys)
        missing = sorted(expected_keys - set(admission))
        raise AdmissionReplayInvalid(
            f"admission field set drifted; extra={extra}, missing={missing}"
        )

    drift = [key for key, value in recomputed.items() if admission.get(key) != value]
    if drift:
        raise AdmissionReplayInvalid(
            "admission JSON differs from raw-artifact recomputation: " + ", ".join(sorted(drift))
        )

    return {
        "schema": REPLAY_SCHEMA,
        "replay_authority": REPLAY_AUTHORITY,
        "replay_gate_passed": True,
        "recomputed_fields": sorted(recomputed),
        "input_artifact_sha256": verified_sha,
    }


def validate_replay_receipt(value: object) -> Mapping[str, object]:
    receipt = _mapping(value, "admission_replay")
    if receipt.get("schema") != REPLAY_SCHEMA:
        raise AdmissionReplayInvalid("admission replay schema mismatch")
    if receipt.get("replay_authority") != REPLAY_AUTHORITY:
        raise AdmissionReplayInvalid("admission replay authority mismatch")
    if receipt.get("replay_gate_passed") is not True:
        raise AdmissionReplayInvalid("admission replay gate did not pass")
    fields = receipt.get("recomputed_fields")
    if not isinstance(fields, list) or not fields or not all(isinstance(item, str) and item for item in fields):
        raise AdmissionReplayInvalid("admission replay recomputed field receipt is invalid")
    hashes = _mapping(receipt.get("input_artifact_sha256"), "admission_replay.input_artifact_sha256")
    if set(hashes) != set(INPUT_ARTIFACT_KEYS):
        raise AdmissionReplayInvalid("admission replay artifact hash set drifted")
    for name in INPUT_ARTIFACT_KEYS:
        _sha256(hashes.get(name), f"admission_replay.input_artifact_sha256.{name}")
    return receipt
