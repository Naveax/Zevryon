from __future__ import annotations

import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys

MAGIC = b"ZMDOC001"
HEADER = struct.Struct("<8sQQQQQQQ")
RECORD_HEADER = struct.Struct("<QI")


def run_generator(tmp_path: Path, *extra: str) -> tuple[Path, dict]:
    output = tmp_path / "corpus.zmdoc"
    command = [
        sys.executable,
        "scripts/generate_massivedoc_corpus.py",
        str(output),
        "--logical-bytes",
        str(2 * 1024 * 1024),
        "--records",
        "1024",
        "--largest-record-limit-bytes",
        str(1024 * 1024),
        "--giant-record-bytes",
        str(1024 * 1024),
        "--stream-chunk-bytes",
        str(64 * 1024),
        *extra,
    ]
    completed = subprocess.run(command, check=True, capture_output=True, text=True)
    return output, json.loads(completed.stdout)


def test_streamed_giant_record_metadata_and_payload_hash(tmp_path: Path) -> None:
    output, summary = run_generator(tmp_path)
    assert summary["largest_record_limit_bytes"] == 1024 * 1024
    assert summary["largest_record_observed_bytes"] == 1024 * 1024
    assert summary["giant_record_bytes"] == 1024 * 1024
    assert summary["average_record_bytes"] == 2048.0
    assert summary["physical_bytes"] == 2 * 1024 * 1024 + HEADER.size + 1024 * RECORD_HEADER.size
    assert summary["container_overhead_bytes"] == HEADER.size + 1024 * RECORD_HEADER.size

    payload_hash = hashlib.sha256()
    observed_sizes: list[int] = []
    with output.open("rb") as stream:
        header = stream.read(HEADER.size)
        magic, logical_bytes, records, *_ = HEADER.unpack(header)
        assert magic == MAGIC
        assert logical_bytes == 2 * 1024 * 1024
        assert records == 1024
        for expected_index in range(records):
            record_header = stream.read(RECORD_HEADER.size)
            logical_id, size = RECORD_HEADER.unpack(record_header)
            assert logical_id == expected_index
            observed_sizes.append(size)
            remaining = size
            while remaining:
                chunk = stream.read(min(64 * 1024, remaining))
                assert chunk
                payload_hash.update(chunk)
                remaining -= len(chunk)
        assert stream.read(1) == b""

    assert max(observed_sizes) == 1024 * 1024
    assert observed_sizes.count(1024 * 1024) == 1
    assert payload_hash.hexdigest() == summary["payload_sha256"]


def test_giant_record_cannot_exceed_configured_limit(tmp_path: Path) -> None:
    output = tmp_path / "invalid.zmdoc"
    completed = subprocess.run(
        [
            sys.executable,
            "scripts/generate_massivedoc_corpus.py",
            str(output),
            "--logical-bytes",
            str(2 * 1024 * 1024),
            "--records",
            "1024",
            "--largest-record-limit-bytes",
            str(512 * 1024),
            "--giant-record-bytes",
            str(1024 * 1024),
        ],
        capture_output=True,
        text=True,
    )
    assert completed.returncode != 0
    assert "exceeds configured record limit" in completed.stderr
