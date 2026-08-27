#!/usr/bin/env python3
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

from generate_massivedoc_corpus import iter_payload_chunks
from m7_synthetic_corpus import (
    CORPUS_CHUNK_BYTES,
    SYNTHETIC_PATTERN,
    canonical_synthetic_chunk,
    iter_synthetic_payload,
    synthetic_corpus_sha256,
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    chunk = canonical_synthetic_chunk()
    require(len(chunk) == CORPUS_CHUNK_BYTES, "canonical synthetic chunk length drifted")
    expected_prefix = (SYNTHETIC_PATTERN * 4)[: min(len(chunk), len(SYNTHETIC_PATTERN) * 4)]
    require(chunk[: len(expected_prefix)] == expected_prefix, "canonical chunk prefix drifted")

    payload_bytes = CORPUS_CHUNK_BYTES + 257
    canonical_parts = list(iter_synthetic_payload(payload_bytes))
    require(len(canonical_parts) == 2, "canonical payload chunk count drifted")
    require(canonical_parts[0] == chunk, "canonical first chunk drifted")
    require(canonical_parts[1] == chunk[:257], "canonical repeated-chunk reset drifted")

    direct_digest = hashlib.sha256(b"".join(canonical_parts)).hexdigest()
    require(
        synthetic_corpus_sha256(payload_bytes) == direct_digest,
        "canonical synthetic SHA does not match emitted bytes",
    )

    legacy = b"".join(
        iter_payload_chunks(
            4,
            payload_bytes,
            chunk_bytes=CORPUS_CHUNK_BYTES,
        )
    )
    require(
        legacy != b"".join(canonical_parts),
        "legacy continuous pattern unexpectedly matches browser repeated-chunk semantics",
    )

    with tempfile.TemporaryDirectory(prefix="zevryon-m7-corpus-") as directory:
        root = Path(directory)
        output = root / "fixture.zmdoc"
        giant_bytes = CORPUS_CHUNK_BYTES + 257
        logical_bytes = giant_bytes + 4096
        command = [
            sys.executable,
            str(Path(__file__).with_name("generate_massivedoc_corpus.py")),
            str(output),
            "--logical-bytes",
            str(logical_bytes),
            "--records",
            "2",
            "--giant-record-bytes",
            str(giant_bytes),
            "--giant-record-index",
            "0",
            "--giant-record-profile",
            "m7-competitor",
        ]
        completed = subprocess.run(
            command,
            check=False,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
        require(completed.returncode == 0, f"M7 corpus generator failed: {completed.stderr}")
        summary = json.loads(output.with_suffix(".zmdoc.json").read_text(encoding="utf-8"))
        expected_sha = synthetic_corpus_sha256(giant_bytes)
        require(summary["giant_record_profile"] == "m7-competitor", "giant profile drifted")
        require(summary["giant_record_sha256"] == expected_sha, "giant record SHA drifted")
        require(
            summary["giant_record_expected_m7_sha256"] == expected_sha,
            "generator expected M7 SHA drifted",
        )
        require(
            summary["giant_record_matches_m7_synthetic"] is True,
            "generator did not prove exact browser corpus parity",
        )

    print("M7 synthetic corpus authority tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
