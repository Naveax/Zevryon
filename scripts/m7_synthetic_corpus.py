#!/usr/bin/env python3
from __future__ import annotations

import hashlib
from typing import Iterator


PAYLOAD_PATTERN_TEXT = "👨‍👩‍👧‍👦👍🏽🚀 "
CORPUS_CHUNK_BYTES = 1024 * 1024
SYNTHETIC_PATTERN = PAYLOAD_PATTERN_TEXT.encode("utf-8")


def canonical_synthetic_chunk() -> bytes:
    repeats = (CORPUS_CHUNK_BYTES + len(SYNTHETIC_PATTERN) - 1) // len(
        SYNTHETIC_PATTERN
    )
    return (SYNTHETIC_PATTERN * repeats)[:CORPUS_CHUNK_BYTES]


def iter_synthetic_payload(payload_bytes: int) -> Iterator[bytes]:
    if payload_bytes <= 0:
        raise ValueError("payload_bytes must be positive")
    chunk = canonical_synthetic_chunk()
    full_chunks, remainder = divmod(payload_bytes, CORPUS_CHUNK_BYTES)
    for _ in range(full_chunks):
        yield chunk
    if remainder:
        yield chunk[:remainder]


def synthetic_corpus_sha256(payload_bytes: int) -> str:
    digest = hashlib.sha256()
    for chunk in iter_synthetic_payload(payload_bytes):
        digest.update(chunk)
    return digest.hexdigest()
