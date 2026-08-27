#!/usr/bin/env python3
from __future__ import annotations

import hashlib
from pathlib import Path
import tempfile

from m7_json_artifact_snapshot import (
    JsonArtifactSnapshotInvalid,
    read_json_object_snapshot,
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def require_invalid(callable_, message: str) -> None:
    try:
        callable_()
    except JsonArtifactSnapshotInvalid:
        return
    raise AssertionError(message)


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="zevryon-m7-json-snapshot-") as temp:
        root = Path(temp)
        valid = root / "valid.json"
        raw = b'{"a":1,"nested":{"b":2}}\n'
        valid.write_bytes(raw)
        snapshot = read_json_object_snapshot(valid, label="valid artifact")
        require(snapshot.sha256 == hashlib.sha256(raw).hexdigest(), "snapshot SHA drifted")
        require(snapshot.byte_count == len(raw), "snapshot byte count drifted")
        require(snapshot.value["a"] == 1, "snapshot JSON value drifted")
        require(snapshot.path == valid, "snapshot path drifted")

        array = root / "array.json"
        array.write_text("[]\n", encoding="utf-8")
        require_invalid(
            lambda: read_json_object_snapshot(array, label="array artifact"),
            "non-object JSON artifact was accepted",
        )

        invalid_json = root / "invalid.json"
        invalid_json.write_text("{broken\n", encoding="utf-8")
        require_invalid(
            lambda: read_json_object_snapshot(invalid_json, label="invalid artifact"),
            "invalid JSON artifact was accepted",
        )

        invalid_utf8 = root / "invalid-utf8.json"
        invalid_utf8.write_bytes(b"\xff\xfe\x00")
        require_invalid(
            lambda: read_json_object_snapshot(invalid_utf8, label="invalid UTF-8 artifact"),
            "non-UTF-8 artifact was accepted",
        )

        missing = root / "missing.json"
        require_invalid(
            lambda: read_json_object_snapshot(missing, label="missing artifact"),
            "missing artifact was accepted",
        )

    print("M7 JSON artifact snapshot tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
