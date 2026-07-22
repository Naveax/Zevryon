#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct
import time
from typing import Iterator

MAGIC = b"ZMDOC001"
HEADER = struct.Struct("<8sQQQQQQQ")
RECORD_HEADER = struct.Struct("<QI")
DEFAULT_STREAM_CHUNK_BYTES = 1024 * 1024

PATTERNS = (
    "a b c d e f g h i j ",
    "İstanbul ıslak IĞDIR normalize e\u0301 ",
    "العربية שלום mixed bidi ",
    "漢字仮名交じり文沒有空格 ",
    "👨‍👩‍👧‍👦👍🏽🚀 ",
    "function f(x){return x*x;} // code\n",
)


def record_size_for(
    index: int,
    *,
    logical_bytes: int,
    records: int,
    giant_record_bytes: int,
    giant_record_index: int,
) -> int:
    if giant_record_bytes:
        if index == giant_record_index:
            return giant_record_bytes
        remaining = logical_bytes - giant_record_bytes
        other_records = records - 1
        base, extra = divmod(remaining, other_records)
        compact_index = index if index < giant_record_index else index - 1
        return base + (1 if compact_index < extra else 0)
    base, extra = divmod(logical_bytes, records)
    return base + (1 if index < extra else 0)


def iter_payload_chunks(
    index: int,
    size: int,
    *,
    tail_marker: bytes = b"",
    chunk_bytes: int = DEFAULT_STREAM_CHUNK_BYTES,
) -> Iterator[bytes]:
    if size <= 0:
        return
    if tail_marker and size < len(tail_marker):
        raise ValueError("record is smaller than tail marker")
    pattern = PATTERNS[index % len(PATTERNS)].encode("utf-8")
    patterned_bytes = size - len(tail_marker)
    offset = 0
    while offset < patterned_bytes:
        take = min(chunk_bytes, patterned_bytes - offset)
        pattern_offset = offset % len(pattern)
        repeat_count = (pattern_offset + take + len(pattern) - 1) // len(pattern)
        expanded = pattern * repeat_count
        yield expanded[pattern_offset:pattern_offset + take]
        offset += take
    if tail_marker:
        yield tail_marker


def main() -> int:
    parser = argparse.ArgumentParser(description="Stream a deterministic MassiveDoc adversarial corpus")
    parser.add_argument("output", type=Path)
    parser.add_argument("--logical-bytes", type=int, required=True)
    parser.add_argument("--records", type=int, required=True)
    parser.add_argument("--logical-nodes", type=int)
    parser.add_argument("--style-runs", type=int)
    parser.add_argument("--resource-references", type=int)
    parser.add_argument("--largest-record-limit-bytes", type=int, default=64 * 1024 * 1024)
    parser.add_argument("--giant-record-bytes", type=int, default=0)
    parser.add_argument("--giant-record-index", type=int)
    parser.add_argument("--stream-chunk-bytes", type=int, default=DEFAULT_STREAM_CHUNK_BYTES)
    parser.add_argument("--tail-marker", default="ZEVRYON_WORST_CASE_TAIL_MARKER")
    args = parser.parse_args()

    if args.logical_bytes <= 0 or args.records <= 0:
        parser.error("logical bytes and records must be positive")
    if args.records > args.logical_bytes:
        parser.error("records cannot exceed bytes because every record must contain at least one byte")
    if args.stream_chunk_bytes <= 0:
        parser.error("stream chunk size must be positive")
    if args.giant_record_bytes < 0:
        parser.error("giant record size cannot be negative")
    if args.giant_record_bytes:
        if args.giant_record_bytes > args.largest_record_limit_bytes:
            parser.error("giant record exceeds configured record limit")
        if args.records < 2:
            parser.error("giant-record mode requires at least two records")
        if args.logical_bytes - args.giant_record_bytes < args.records - 1:
            parser.error("remaining bytes cannot provide at least one byte to every other record")
        giant_index = args.giant_record_index
        if giant_index is None:
            giant_index = args.records // 2
        if not 0 <= giant_index < args.records:
            parser.error("giant record index is outside the record range")
    else:
        giant_index = 0

    nodes = args.logical_nodes if args.logical_nodes is not None else args.records * 8
    styles = args.style_runs if args.style_runs is not None else args.records * 4
    resources = args.resource_references if args.resource_references is not None else max(1, args.records // 8)
    largest_observed = max(
        record_size_for(
            0,
            logical_bytes=args.logical_bytes,
            records=args.records,
            giant_record_bytes=args.giant_record_bytes,
            giant_record_index=giant_index,
        ),
        args.giant_record_bytes,
    )
    if largest_observed > args.largest_record_limit_bytes:
        parser.error("requested record distribution exceeds configured record limit")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    container_hasher = hashlib.sha256()
    payload_hasher = hashlib.sha256()
    tail_marker = args.tail_marker.encode("utf-8")
    started = time.perf_counter()
    with args.output.open("wb") as stream:
        header = HEADER.pack(
            MAGIC,
            args.logical_bytes,
            args.records,
            nodes,
            styles,
            resources,
            args.largest_record_limit_bytes,
            0,
        )
        stream.write(header)
        container_hasher.update(header)
        for index in range(args.records):
            size = record_size_for(
                index,
                logical_bytes=args.logical_bytes,
                records=args.records,
                giant_record_bytes=args.giant_record_bytes,
                giant_record_index=giant_index,
            )
            marker = tail_marker if index + 1 == args.records else b""
            record_header = RECORD_HEADER.pack(index, size)
            stream.write(record_header)
            container_hasher.update(record_header)
            for chunk in iter_payload_chunks(
                index,
                size,
                tail_marker=marker,
                chunk_bytes=args.stream_chunk_bytes,
            ):
                stream.write(chunk)
                container_hasher.update(chunk)
                payload_hasher.update(chunk)
    elapsed = time.perf_counter() - started
    physical_bytes = args.output.stat().st_size
    summary = {
        "format": "ZMDOC001",
        "path": str(args.output),
        "logical_utf8_bytes": args.logical_bytes,
        "logical_records": args.records,
        "logical_nodes": nodes,
        "style_runs": styles,
        "resource_references": resources,
        "largest_record_limit_bytes": args.largest_record_limit_bytes,
        "largest_record_observed_bytes": largest_observed,
        "average_record_bytes": args.logical_bytes / args.records,
        "giant_record_bytes": args.giant_record_bytes,
        "giant_record_index": giant_index if args.giant_record_bytes else None,
        "stream_chunk_bytes": args.stream_chunk_bytes,
        "physical_bytes": physical_bytes,
        "container_overhead_bytes": physical_bytes - args.logical_bytes,
        "container_sha256": container_hasher.hexdigest(),
        "payload_sha256": payload_hasher.hexdigest(),
        "tail_marker": args.tail_marker,
        "generation_seconds": elapsed,
        "throughput_mb_s_decimal": args.logical_bytes / 1_000_000 / elapsed if elapsed else 0,
    }
    args.output.with_suffix(args.output.suffix + ".json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(json.dumps(summary, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
