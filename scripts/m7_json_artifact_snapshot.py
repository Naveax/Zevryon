#!/usr/bin/env python3
from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
from typing import Mapping


class JsonArtifactSnapshotInvalid(ValueError):
    pass


@dataclass(frozen=True)
class JsonArtifactSnapshot:
    path: Path
    sha256: str
    byte_count: int
    value: Mapping[str, object]


def read_json_object_snapshot(path: Path, *, label: str) -> JsonArtifactSnapshot:
    try:
        raw = path.read_bytes()
    except OSError as exc:
        raise JsonArtifactSnapshotInvalid(f"cannot read {label}: {exc}") from exc

    digest = hashlib.sha256(raw).hexdigest()
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise JsonArtifactSnapshotInvalid(f"{label} is not UTF-8") from exc
    try:
        value = json.loads(text)
    except json.JSONDecodeError as exc:
        raise JsonArtifactSnapshotInvalid(f"cannot parse {label}: {exc}") from exc
    if not isinstance(value, Mapping):
        raise JsonArtifactSnapshotInvalid(f"{label} must be a JSON object")

    return JsonArtifactSnapshot(
        path=path,
        sha256=digest,
        byte_count=len(raw),
        value=value,
    )
