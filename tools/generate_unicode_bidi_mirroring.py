#!/usr/bin/env python3
"""Generate compact Unicode 17 bidi-mirroring data.

The normative Bidi_Mirrored property comes from UnicodeData.txt field 9.
The informative Bidi_Mirroring_Glyph mapping comes from BidiMirroring.txt.
They are deliberately represented separately so a renderer never mistakes an
absent character mapping for Bidi_Mirrored=No.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

UNICODE_VERSION = "17.0.0"


@dataclass(frozen=True, order=True)
class MirrorMapping:
    codepoint: int
    mirror_codepoint: int
    best_fit: bool


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def parse_unicode_data(path: Path) -> list[int]:
    mirrored: list[int] = []
    pending_range: tuple[int, bool] | None = None

    for line_number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not raw:
            continue
        fields = raw.split(";")
        if len(fields) != 15:
            raise ValueError(f"UnicodeData line {line_number}: expected 15 fields")
        codepoint = int(fields[0], 16)
        name = fields[1]
        is_mirrored = fields[9] == "Y"
        if fields[9] not in {"Y", "N"}:
            raise ValueError(f"UnicodeData line {line_number}: invalid mirrored flag")

        if name.endswith(", First>"):
            if pending_range is not None:
                raise ValueError(f"UnicodeData line {line_number}: nested range")
            pending_range = (codepoint, is_mirrored)
            continue
        if name.endswith(", Last>"):
            if pending_range is None:
                raise ValueError(f"UnicodeData line {line_number}: unmatched range end")
            first, range_mirrored = pending_range
            if codepoint < first or range_mirrored != is_mirrored:
                raise ValueError(f"UnicodeData line {line_number}: inconsistent range")
            if is_mirrored:
                mirrored.extend(range(first, codepoint + 1))
            pending_range = None
            continue
        if pending_range is not None:
            raise ValueError(f"UnicodeData line {line_number}: unterminated range")
        if is_mirrored:
            mirrored.append(codepoint)

    if pending_range is not None:
        raise ValueError("UnicodeData: unterminated final range")
    if mirrored != sorted(set(mirrored)):
        raise ValueError("UnicodeData: mirrored codepoints are not unique and sorted")
    return mirrored


def parse_bidi_mirroring(path: Path) -> list[MirrorMapping]:
    mappings: list[MirrorMapping] = []
    for line_number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        body, _, comment = raw.partition("#")
        body = body.strip()
        if not body:
            continue
        fields = [field.strip() for field in body.split(";")]
        if len(fields) != 2:
            raise ValueError(f"BidiMirroring line {line_number}: expected two fields")
        mappings.append(
            MirrorMapping(
                codepoint=int(fields[0], 16),
                mirror_codepoint=int(fields[1], 16),
                best_fit="BEST FIT" in comment,
            )
        )
    mappings.sort()
    if len({mapping.codepoint for mapping in mappings}) != len(mappings):
        raise ValueError("BidiMirroring: duplicate source codepoint")
    return mappings


def contiguous_ranges(values: Iterable[int]) -> list[tuple[int, int]]:
    values = list(values)
    if not values:
        return []
    ranges: list[tuple[int, int]] = []
    first = previous = values[0]
    for value in values[1:]:
        if value == previous + 1:
            previous = value
            continue
        ranges.append((first, previous))
        first = previous = value
    ranges.append((first, previous))
    return ranges


def normalized_fingerprint(
    mirrored: list[int], mappings: list[MirrorMapping]
) -> str:
    digest = hashlib.sha256()
    for codepoint in mirrored:
        digest.update(f"M;{codepoint:06X}\n".encode("ascii"))
    for mapping in mappings:
        digest.update(
            f"G;{mapping.codepoint:06X};{mapping.mirror_codepoint:06X};"
            f"{int(mapping.best_fit)}\n".encode("ascii")
        )
    return digest.hexdigest()


def render_header(
    mirrored: list[int],
    mappings: list[MirrorMapping],
    unicode_data_sha: str,
    mirroring_sha: str,
    fingerprint: str,
) -> str:
    ranges = contiguous_ranges(mirrored)
    range_rows = "\n".join(
        f"    BidiMirroredRange{{0x{first:X}U, 0x{last:X}U}},"
        for first, last in ranges
    )
    mapping_rows = "\n".join(
        "    BidiMirroringGlyphRecord{"
        f"0x{mapping.codepoint:X}U, 0x{mapping.mirror_codepoint:X}U, "
        f"{'true' if mapping.best_fit else 'false'}" + "},"
        for mapping in mappings
    )
    return f'''#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace zevryon::text {{

struct BidiMirroredRange {{
    std::uint32_t first{{0}};
    std::uint32_t last{{0}};
}};

struct BidiMirroringGlyphRecord {{
    std::uint32_t codepoint{{0}};
    std::uint32_t mirror_codepoint{{0}};
    bool best_fit{{false}};
}};

inline constexpr std::string_view kUnicodeBidiMirroringVersion = "{UNICODE_VERSION}";
inline constexpr std::string_view kUnicodeDataSourceSha256 = "{unicode_data_sha}";
inline constexpr std::string_view kUnicodeBidiMirroringSourceSha256 = "{mirroring_sha}";
inline constexpr std::string_view kUnicodeBidiMirroringFingerprint = "{fingerprint}";

inline constexpr std::array<BidiMirroredRange, {len(ranges)}> kUnicodeBidiMirroredRanges{{{{
{range_rows}
}}}};

inline constexpr std::array<BidiMirroringGlyphRecord, {len(mappings)}> kUnicodeBidiMirroringGlyphs{{{{
{mapping_rows}
}}}};

}} // namespace zevryon::text
'''


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--unicode-data", type=Path, required=True)
    parser.add_argument("--bidi-mirroring", type=Path, required=True)
    parser.add_argument("--output-header", type=Path, required=True)
    parser.add_argument("--output-report", type=Path, required=True)
    args = parser.parse_args()

    mirrored = parse_unicode_data(args.unicode_data)
    mappings = parse_bidi_mirroring(args.bidi_mirroring)
    mirrored_set = set(mirrored)
    missing_sources = [
        mapping.codepoint for mapping in mappings if mapping.codepoint not in mirrored_set
    ]
    if missing_sources:
        raise ValueError(
            "BidiMirroring contains sources without Bidi_Mirrored=Y: "
            + ", ".join(f"U+{value:04X}" for value in missing_sources[:8])
        )

    unicode_data_sha = sha256_file(args.unicode_data)
    mirroring_sha = sha256_file(args.bidi_mirroring)
    fingerprint = normalized_fingerprint(mirrored, mappings)
    ranges = contiguous_ranges(mirrored)
    mapped_sources = {mapping.codepoint for mapping in mappings}

    args.output_header.parent.mkdir(parents=True, exist_ok=True)
    args.output_report.parent.mkdir(parents=True, exist_ok=True)
    args.output_header.write_text(
        render_header(
            mirrored,
            mappings,
            unicode_data_sha,
            mirroring_sha,
            fingerprint,
        ),
        encoding="utf-8",
        newline="\n",
    )
    report = {
        "schema": "zevryon.unicode-bidi-mirroring-generator.v1",
        "unicode_version": UNICODE_VERSION,
        "unicode_data_sha256": unicode_data_sha,
        "bidi_mirroring_sha256": mirroring_sha,
        "normalized_fingerprint": fingerprint,
        "mirrored_codepoints": len(mirrored),
        "mirrored_ranges": len(ranges),
        "mapping_records": len(mappings),
        "best_fit_records": sum(mapping.best_fit for mapping in mappings),
        "mirrored_without_character_mapping": len(mirrored_set - mapped_sources),
        "minimum_mirrored_codepoint": mirrored[0],
        "maximum_mirrored_codepoint": mirrored[-1],
        "passed": True,
    }
    args.output_report.write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
        newline="\n",
    )
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
