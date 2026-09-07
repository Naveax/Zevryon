#!/usr/bin/env python3
from __future__ import annotations

"""UTF-8-safe implementation patch for the admitted M8 Titan fixture authority.

The frozen Titan envelope/report schema remains unchanged.  Only ordinary-record
payload construction is replaced so arbitrary exact record byte sizes cannot
terminate in the middle of a UTF-8 code point.  Special giant/token/grapheme
records continue to use the admitted implementation unchanged.
"""

import hashlib
from pathlib import Path
import sys
from typing import Iterator

SCRIPT_ROOT = Path(__file__).resolve().parent
if str(SCRIPT_ROOT) not in sys.path:
    sys.path.insert(0, str(SCRIPT_ROOT))

import m8_titan_fixture as base
from generate_massivedoc_corpus import HEADER, MAGIC, PATTERNS, RECORD_HEADER

UTF8_IMPLEMENTATION = "m8-titan-ordinary-record-utf8-safe-v1"


def _largest_valid_prefix(pattern: bytes, maximum: int) -> bytes:
    candidate = pattern[:maximum]
    while candidate:
        try:
            candidate.decode("utf-8", errors="strict")
            return candidate
        except UnicodeDecodeError:
            candidate = candidate[:-1]
    return b""


def ordinary_utf8_chunks(
    index: int,
    size: int,
    *,
    tail_marker: bytes = b"",
) -> Iterator[bytes]:
    base.require(index >= base.SPECIAL_RECORD_COUNT, "ordinary UTF-8 record overlaps a special record")
    base.require(size >= len(tail_marker), "ordinary record is smaller than its tail marker")
    if not size:
        return

    pattern = PATTERNS[index % len(PATTERNS)].encode("utf-8")
    body_bytes = size - len(tail_marker)
    complete_pattern_bytes = (body_bytes // len(pattern)) * len(pattern)
    if complete_pattern_bytes:
        # Chunk boundaries may split a code point, which is fine: the concatenated
        # record remains exact repetitions of the complete UTF-8 pattern.
        yield from base.repeat_chunks(pattern, complete_pattern_bytes)

    remainder = body_bytes - complete_pattern_bytes
    if remainder:
        prefix = _largest_valid_prefix(pattern, remainder)
        if prefix:
            yield prefix
        filler = remainder - len(prefix)
        if filler:
            # ASCII filler preserves the exact byte count without manufacturing a
            # partial multi-byte sequence at the record boundary.
            yield b"x" * filler
    if tail_marker:
        # Frozen marker is ASCII and therefore keeps the record UTF-8 valid.
        yield tail_marker


def generate_fixture_utf8(
    output: Path,
    config: base.TitanConfig,
) -> tuple[dict[str, object], dict[int, dict[str, object]]]:
    output.parent.mkdir(parents=True, exist_ok=True)
    payload_hash = hashlib.sha256()
    container_hash = hashlib.sha256()
    special_hashers = {
        base.GIANT_RECORD_INDEX: hashlib.sha256(),
        base.TOKEN_RECORD_INDEX: hashlib.sha256(),
        base.GRAPHEME_RECORD_INDEX: hashlib.sha256(),
    }
    receipts: dict[int, dict[str, object]] = {}
    header = HEADER.pack(
        MAGIC,
        config.logical_bytes,
        config.records,
        config.logical_nodes,
        config.style_runs,
        config.resource_references,
        config.giant_record_bytes,
        0,
    )
    try:
        stream = output.open("xb")
    except FileExistsError as exc:
        raise base.TitanEvidenceInvalid(f"refusing to overwrite Titan corpus: {output}") from exc

    with stream:
        stream.write(header)
        container_hash.update(header)
        for index in range(config.records):
            size = base.record_size(index, config)
            header_offset = stream.tell()
            raw_record_header = RECORD_HEADER.pack(index, size)
            stream.write(raw_record_header)
            container_hash.update(raw_record_header)
            payload_offset = stream.tell()
            if index < base.SPECIAL_RECORD_COUNT:
                chunks = base.special_chunks(index, size)
            else:
                chunks = ordinary_utf8_chunks(
                    index,
                    size,
                    tail_marker=base.TAIL_MARKER if index + 1 == config.records else b"",
                )
            written = 0
            for chunk in chunks:
                if not chunk:
                    continue
                stream.write(chunk)
                payload_hash.update(chunk)
                container_hash.update(chunk)
                written += len(chunk)
                if index in special_hashers:
                    special_hashers[index].update(chunk)
            base.require(written == size, f"record {index} byte count drifted")
            if index in special_hashers:
                receipts[index] = {
                    "record_index": index,
                    "record_header_offset": header_offset,
                    "payload_offset": payload_offset,
                    "payload_bytes": size,
                    "sha256": special_hashers[index].hexdigest(),
                }

    physical = output.stat().st_size
    base.require(physical == base.expected_physical_bytes(config), "Titan corpus physical byte count drifted")
    return {
        "container_sha256": container_hash.hexdigest(),
        "payload_sha256": payload_hash.hexdigest(),
        "physical_bytes": physical,
    }, receipts


def verify_fixture_utf8(
    output: Path,
    config: base.TitanConfig,
    receipts: dict[int, dict[str, object]],
) -> dict[str, object]:
    verification = _ORIGINAL_VERIFY(output, config, receipts)
    verification["ordinary_record_utf8_implementation"] = UTF8_IMPLEMENTATION
    verification["ordinary_records_utf8_safe_by_construction"] = True
    return verification


_ORIGINAL_VERIFY = base.verify_fixture
base.generate_fixture = generate_fixture_utf8
base.verify_fixture = verify_fixture_utf8


if __name__ == "__main__":
    raise SystemExit(base.main())
