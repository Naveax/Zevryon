#!/usr/bin/env python3
"""Generate deterministic compact Unicode Bidi_Paired_Bracket tables."""

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
    blob = b"".join(
        codepoint.to_bytes(4, "little")
        + paired.to_bytes(4, "little")
        + bytes((1 if bracket_type == "o" else 2,))
        for codepoint, paired, bracket_type in rows
    )
    header = f'''#pragma once

#include <array>
#include <cstddef>
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
inline constexpr std::string_view kUnicodeBidiBracketHex =
    "{blob.hex()}";

constexpr std::uint8_t bidi_bracket_hex_nibble(char value) noexcept {{
    return value >= '0' && value <= '9'
        ? static_cast<std::uint8_t>(value - '0')
        : static_cast<std::uint8_t>(value - 'a' + 10);
}}

constexpr std::uint8_t bidi_bracket_hex_byte(std::size_t offset) noexcept {{
    return static_cast<std::uint8_t>(
        (bidi_bracket_hex_nibble(kUnicodeBidiBracketHex[offset]) << 4U) |
        bidi_bracket_hex_nibble(kUnicodeBidiBracketHex[offset + 1U]));
}}

constexpr std::uint32_t bidi_bracket_u32(std::size_t byte_offset) noexcept {{
    const std::size_t hex_offset = byte_offset * 2U;
    return static_cast<std::uint32_t>(bidi_bracket_hex_byte(hex_offset)) |
           (static_cast<std::uint32_t>(bidi_bracket_hex_byte(hex_offset + 2U)) << 8U) |
           (static_cast<std::uint32_t>(bidi_bracket_hex_byte(hex_offset + 4U)) << 16U) |
           (static_cast<std::uint32_t>(bidi_bracket_hex_byte(hex_offset + 6U)) << 24U);
}}

constexpr std::array<BidiBracketRecord, {len(rows)}> decode_bidi_bracket_records() noexcept {{
    std::array<BidiBracketRecord, {len(rows)}> output{{}};
    for (std::size_t index = 0U; index < output.size(); ++index) {{
        const std::size_t byte_offset = index * 9U;
        output[index] = BidiBracketRecord{{
            bidi_bracket_u32(byte_offset),
            bidi_bracket_u32(byte_offset + 4U),
            static_cast<BidiBracketType>(
                bidi_bracket_hex_byte((byte_offset + 8U) * 2U))}};
    }}
    return output;
}}

inline constexpr auto kUnicodeBidiBracketRecords = decode_bidi_bracket_records();

static_assert(kUnicodeBidiBracketHex.size() == {len(rows)}U * 9U * 2U);
static_assert(kUnicodeBidiBracketRecords.front().codepoint == 0x28U);
static_assert(kUnicodeBidiBracketRecords.back().codepoint == 0xFF63U);

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
