#!/usr/bin/env python3
"""Generate deterministic Unicode Bidi_Paired_Bracket tables."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

UNICODE_VERSION = "17.0.0"
MAX_CODEPOINT = 0x10FFFF


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def parse(path: Path) -> list[tuple[int, int, str]]:
    rows: list[tuple[int, int, str]] = []
    seen: set[int] = set()
    for line_number, raw_line in enumerate(
        path.read_text(encoding="utf-8").splitlines(), start=1
    ):
        content = raw_line.split("#", 1)[0].strip()
        if not content:
            continue
        fields = [field.strip() for field in content.split(";")]
        if len(fields) != 3:
            raise ValueError(f"invalid BidiBrackets line {line_number}: {raw_line}")
        codepoint = int(fields[0], 16)
        paired = int(fields[1], 16)
        bracket_type = fields[2]
        if codepoint > MAX_CODEPOINT or paired > MAX_CODEPOINT:
            raise ValueError(f"out-of-range codepoint on line {line_number}")
        if bracket_type not in {"o", "c"}:
            raise ValueError(f"invalid bracket type on line {line_number}")
        if codepoint in seen:
            raise ValueError(f"duplicate bracket codepoint U+{codepoint:04X}")
        seen.add(codepoint)
        rows.append((codepoint, paired, bracket_type))
    rows.sort()
    mapping = {codepoint: (paired, bracket_type) for codepoint, paired, bracket_type in rows}
    for codepoint, paired, bracket_type in rows:
        reverse = mapping.get(paired)
        expected_reverse = "c" if bracket_type == "o" else "o"
        if reverse != (codepoint, expected_reverse):
            raise ValueError(f"non-reciprocal bracket mapping U+{codepoint:04X}")
    return rows


def generate(path: Path) -> tuple[str, dict[str, object]]:
    rows = parse(path)
    canonical = {
        "version": UNICODE_VERSION,
        "rows": [[a, b, t] for a, b, t in rows],
    }
    fingerprint = hashlib.sha256(
        json.dumps(canonical, separators=(",", ":"), sort_keys=True).encode("utf-8")
    ).hexdigest()
    entries = "\n".join(
        "    BidiBracketRecord{0x%XU, 0x%XU, BidiBracketType::%s},"
        % (codepoint, paired, "Open" if bracket_type == "o" else "Close")
        for codepoint, paired, bracket_type in rows
    )
    header = f'''#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace zevryon::text {{

enum class BidiBracketType : std::uint8_t {{
    None = 0,
    Open,
    Close
}};

struct BidiBracketRecord {{
    std::uint32_t codepoint{{0}};
    std::uint32_t paired_codepoint{{0}};
    BidiBracketType type{{BidiBracketType::None}};
}};

inline constexpr std::string_view kUnicodeBidiBracketVersion = "{UNICODE_VERSION}";
inline constexpr std::string_view kUnicodeBidiBracketFingerprint = "{fingerprint}";
inline constexpr std::string_view kUnicodeBidiBracketSourceSha256 = "{sha256(path)}";
inline constexpr std::array<BidiBracketRecord, {len(rows)}> kUnicodeBidiBracketRecords{{{{
{entries}
}}}};

}} // namespace zevryon::text
'''
    metadata = {
        "schema": "zevryon.unicode-bidi-brackets.v1",
        "unicode_version": UNICODE_VERSION,
        "source_sha256": sha256(path),
        "fingerprint": fingerprint,
        "records": len(rows),
        "open": sum(1 for _, _, value in rows if value == "o"),
        "close": sum(1 for _, _, value in rows if value == "c"),
    }
    return header, metadata


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("bidi_brackets", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--metadata", type=Path)
    args = parser.parse_args()
    header, metadata = generate(args.bidi_brackets)
    args.output.write_text(header, encoding="utf-8", newline="\n")
    if args.metadata is not None:
        args.metadata.write_text(
            json.dumps(metadata, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
            newline="\n",
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
