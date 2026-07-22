#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct
import time

MAGIC = b"ZMDOC001"
HEADER = struct.Struct("<8sQQQQQQQ")
RECORD_HEADER = struct.Struct("<QI")

PATTERNS = (
    "a b c d e f g h i j ",
    "İstanbul ıslak IĞDIR normalize e\u0301 ",
    "العربية שלום mixed bidi ",
    "漢字仮名交じり文沒有空格 ",
    "👨‍👩‍👧‍👦👍🏽🚀 ",
    "function f(x){return x*x;} // code\n",
)


def payload_for(index: int, size: int, tail_marker: bytes = b"") -> bytes:
    if size <= 0:
        return b""
    pattern = PATTERNS[index % len(PATTERNS)].encode("utf-8")
    repeat, remainder = divmod(size, len(pattern))
    payload = pattern * repeat + pattern[:remainder]
    if tail_marker and size >= len(tail_marker):
        payload = payload[:-len(tail_marker)] + tail_marker
    return payload


def main() -> int:
    parser = argparse.ArgumentParser(description="Stream a deterministic MassiveDoc adversarial corpus")
    parser.add_argument("output", type=Path)
    parser.add_argument("--logical-bytes", type=int, required=True)
    parser.add_argument("--records", type=int, required=True)
    parser.add_argument("--logical-nodes", type=int)
    parser.add_argument("--style-runs", type=int)
    parser.add_argument("--resource-references", type=int)
    parser.add_argument("--largest-record-bytes", type=int, default=64 * 1024 * 1024)
    parser.add_argument("--tail-marker", default="ZEVRYON_WORST_CASE_TAIL_MARKER")
    args = parser.parse_args()
    if args.logical_bytes <= 0 or args.records <= 0:
        parser.error("logical bytes and records must be positive")
    if args.records > args.logical_bytes:
        parser.error("records cannot exceed bytes because every record must contain at least one byte")

    nodes = args.logical_nodes if args.logical_nodes is not None else args.records * 8
    styles = args.style_runs if args.style_runs is not None else args.records * 4
    resources = args.resource_references if args.resource_references is not None else max(1, args.records // 8)
    base, extra = divmod(args.logical_bytes, args.records)
    if base + (1 if extra else 0) > args.largest_record_bytes:
        parser.error("requested distribution exceeds largest-record limit; increase records or the limit")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    hasher = hashlib.sha256()
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
            args.largest_record_bytes,
            0,
        )
        stream.write(header)
        hasher.update(header)
        for index in range(args.records):
            size = base + (1 if index < extra else 0)
            marker = tail_marker if index + 1 == args.records else b""
            payload = payload_for(index, size, marker)
            record_header = RECORD_HEADER.pack(index, size)
            stream.write(record_header)
            stream.write(payload)
            hasher.update(record_header)
            hasher.update(payload)
            payload_hasher.update(payload)
    elapsed = time.perf_counter() - started
    summary = {
        "format": "ZMDOC001",
        "path": str(args.output),
        "logical_utf8_bytes": args.logical_bytes,
        "logical_records": args.records,
        "logical_nodes": nodes,
        "style_runs": styles,
        "resource_references": resources,
        "largest_record_bytes": args.largest_record_bytes,
        "physical_bytes": args.output.stat().st_size,
        "container_sha256": hasher.hexdigest(),
        "payload_sha256": payload_hasher.hexdigest(),
        "tail_marker": args.tail_marker,
        "generation_seconds": elapsed,
        "throughput_mib_s": args.logical_bytes / (1024 * 1024) / elapsed if elapsed else 0,
    }
    args.output.with_suffix(args.output.suffix + ".json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(json.dumps(summary, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
