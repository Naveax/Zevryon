#!/usr/bin/env python3
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re
from typing import Mapping

from m7_admission_replay import resolve_artifact_path
from m7_collection_admission_v3 import (
    CollectionAdmissionInvalid,
    admit_collection,
)
from m7_json_artifact_snapshot import (
    JsonArtifactSnapshotInvalid,
    read_json_object_snapshot,
)


REPLAY_SCHEMA = "zevryon.competitor.collection-admission-replay.v2"
REPLAY_AUTHORITY = "m7-atomic-raw-artifacts-recompute-admission-v2"
INPUT_ARTIFACT_KEYS = (
    "preflight",
    "browser_report",
    "zevryon_virtualized",
    "zevryon_native_dom",
)
RECOMPUTED_ADMISSION_FIELDS = (
    "admission_authority",
    "corpus_sha256",
    "leadership_eligible",
    "leadership_evaluation",
    "leadership_metric_gate_evaluated",
    "physical_host_evidence",
    "runtime_bindings",
    "schema",
    "system_fingerprint",
)
_SHA256_RE = re.compile(r"^[0-9a-f]{64}$")


class AdmissionReplayInvalid(ValueError):
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


def _byte_count(value: object, field: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
        raise AdmissionReplayInvalid(f"{field} must be a positive integer")
    return value


def recomputed_admission_payload(admission: Mapping[str, object]) -> dict[str, object]:
    missing = [field for field in RECOMPUTED_ADMISSION_FIELDS if field not in admission]
    if missing:
        raise AdmissionReplayInvalid(
            "admission lacks recomputed authority fields: " + ", ".join(missing)
        )
    return {field: admission[field] for field in RECOMPUTED_ADMISSION_FIELDS}


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
    verified_bytes: dict[str, int] = {}
    for name in INPUT_ARTIFACT_KEYS:
        receipt = _mapping(receipts.get(name), f"input_artifacts.{name}")
        declared = Path(_text(receipt.get("path"), f"input_artifacts.{name}.path"))
        expected_sha = _sha256(
            receipt.get("sha256"),
            f"input_artifacts.{name}.sha256",
        )
        expected_bytes = _byte_count(
            receipt.get("byte_count"),
            f"input_artifacts.{name}.byte_count",
        )
        try:
            resolved = resolve_artifact_path(artifact_root, declared)
            snapshot = read_json_object_snapshot(resolved, label=name)
        except (ValueError, JsonArtifactSnapshotInvalid) as exc:
            raise AdmissionReplayInvalid(f"raw artifact invalid: {name}: {exc}") from exc
        if snapshot.sha256 != expected_sha:
            raise AdmissionReplayInvalid(
                f"raw artifact SHA drifted: {name}; expected={expected_sha}, actual={snapshot.sha256}"
            )
        if snapshot.byte_count != expected_bytes:
            raise AdmissionReplayInvalid(
                f"raw artifact byte count drifted: {name}; expected={expected_bytes}, actual={snapshot.byte_count}"
            )
        raw[name] = snapshot.value
        verified_sha[name] = snapshot.sha256
        verified_bytes[name] = snapshot.byte_count

    try:
        recomputed = admit_collection(
            raw["preflight"],
            raw["browser_report"],
            raw["zevryon_virtualized"],
            raw["zevryon_native_dom"],
        )
    except CollectionAdmissionInvalid as exc:
        raise AdmissionReplayInvalid(
            f"raw artifacts no longer reproduce a valid v3 collection admission: {exc}"
        ) from exc

    if set(recomputed) != set(RECOMPUTED_ADMISSION_FIELDS):
        raise AdmissionReplayInvalid(
            "recomputed v3 admission authority field set drifted from replay contract"
        )
    expected_keys = set(RECOMPUTED_ADMISSION_FIELDS) | {"input_artifacts"}
    if set(admission) != expected_keys:
        extra = sorted(set(admission) - expected_keys)
        missing = sorted(expected_keys - set(admission))
        raise AdmissionReplayInvalid(
            f"admission field set drifted; extra={extra}, missing={missing}"
        )

    drift = [key for key, value in recomputed.items() if admission.get(key) != value]
    if drift:
        raise AdmissionReplayInvalid(
            "admission JSON differs from atomic raw-artifact recomputation: "
            + ", ".join(sorted(drift))
        )

    recomputed_payload = recomputed_admission_payload(recomputed)
    return {
        "schema": REPLAY_SCHEMA,
        "replay_authority": REPLAY_AUTHORITY,
        "replay_gate_passed": True,
        "recomputed_fields": list(RECOMPUTED_ADMISSION_FIELDS),
        "recomputed_admission_sha256": canonical_sha256(recomputed_payload),
        "input_artifact_sha256": verified_sha,
        "input_artifact_byte_count": verified_bytes,
    }


def validate_replay_receipt(value: object) -> Mapping[str, object]:
    receipt = _mapping(value, "admission_replay")
    if receipt.get("schema") != REPLAY_SCHEMA:
        raise AdmissionReplayInvalid("admission replay schema mismatch")
    if receipt.get("replay_authority") != REPLAY_AUTHORITY:
        raise AdmissionReplayInvalid("admission replay authority mismatch")
    if receipt.get("replay_gate_passed") is not True:
        raise AdmissionReplayInvalid("admission replay gate did not pass")
    if receipt.get("recomputed_fields") != list(RECOMPUTED_ADMISSION_FIELDS):
        raise AdmissionReplayInvalid("admission replay recomputed field set drifted")
    _sha256(
        receipt.get("recomputed_admission_sha256"),
        "admission_replay.recomputed_admission_sha256",
    )

    hashes = _mapping(
        receipt.get("input_artifact_sha256"),
        "admission_replay.input_artifact_sha256",
    )
    counts = _mapping(
        receipt.get("input_artifact_byte_count"),
        "admission_replay.input_artifact_byte_count",
    )
    if set(hashes) != set(INPUT_ARTIFACT_KEYS) or set(counts) != set(INPUT_ARTIFACT_KEYS):
        raise AdmissionReplayInvalid("admission replay artifact receipt set drifted")
    for name in INPUT_ARTIFACT_KEYS:
        _sha256(hashes.get(name), f"admission_replay.input_artifact_sha256.{name}")
        _byte_count(counts.get(name), f"admission_replay.input_artifact_byte_count.{name}")
    return receipt
