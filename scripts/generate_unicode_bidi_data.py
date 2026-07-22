#!/usr/bin/env python3
"""Generate deterministic Unicode 17 bidi-class, bracket and mirror tables."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path

UNICODE_VERSION = "17.0.0"
MAX_CODEPOINT = 0x10FFFF
BIDI_CLASS_ORDER = [
    "L", "R", "AL", "EN", "ES", "ET", "AN", "CS", "NSM", "BN",
    "B", "S", "WS", "ON", "LRE", "LRO", "RLE", "RLO", "PDF",
    "LRI", "RLI", "FSI", "PDI",
]


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def parse_range(token: str) -> tuple[int, int]:
    pieces = token.strip().split("..")
    start = int(pieces[0], 16)
    end = int(pieces[-1], 16)
    if start > end or end > MAX_CODEPOINT:
        raise ValueError(f"invalid Unicode range: {token}")
    return start, end


def parse_aliases(path: Path) -> tuple[dict[str, str], dict[str, str]]:
    long_to_short: dict[str, str] = {}
    short_to_long: dict[str, str] = {}
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        content = raw_line.split("#", 1)[0].strip()
        if not content:
            continue
        fields = [field.strip() for field in content.split(";")]
        if len(fields) < 3 or fields[0] != "bc":
            continue
        short_name, long_name = fields[1], fields[2]
        long_to_short[short_name] = short_name
        long_to_short[long_name] = short_name
        short_to_long[short_name] = long_name
        for alias in fields[3:]:
            if alias and alias != "n/a":
                long_to_short[alias] = short_name
    if set(BIDI_CLASS_ORDER) != set(short_to_long):
        missing = set(BIDI_CLASS_ORDER) - set(short_to_long)
        extra = set(short_to_long) - set(BIDI_CLASS_ORDER)
        raise ValueError(f"unexpected Bidi_Class aliases: missing={missing}, extra={extra}")
    return long_to_short, short_to_long


def normalize_class(value: str, aliases: dict[str, str]) -> str:
    try:
        return aliases[value.strip()]
    except KeyError as exc:
        raise ValueError(f"unknown Bidi_Class value: {value}") from exc


def parse_derived_bidi(
    path: Path,
    aliases: dict[str, str],
) -> list[tuple[int, int, str]]:
    missing_rules: list[tuple[int, int, str]] = []
    explicit_rules: list[tuple[int, int, str]] = []
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        if "@missing:" in raw_line:
            content = raw_line.split("@missing:", 1)[1].split("#", 1)[0].strip()
            left, separator, right = content.partition(";")
            if not separator:
                raise ValueError(f"invalid @missing line: {raw_line}")
            start, end = parse_range(left)
            missing_rules.append((start, end, normalize_class(right, aliases)))
            continue
        content = raw_line.split("#", 1)[0].strip()
        if not content:
            continue
        left, separator, right = content.partition(";")
        if not separator:
            raise ValueError(f"invalid DerivedBidiClass line: {raw_line}")
        start, end = parse_range(left)
        explicit_rules.append((start, end, normalize_class(right, aliases)))

    values = ["L"] * (MAX_CODEPOINT + 1)
    for start, end, bidi_class in missing_rules:
        values[start : end + 1] = [bidi_class] * (end - start + 1)
    for start, end, bidi_class in explicit_rules:
        values[start : end + 1] = [bidi_class] * (end - start + 1)

    ranges: list[tuple[int, int, str]] = []
    start = 0
    current = values[0]
    for codepoint in range(1, MAX_CODEPOINT + 1):
        value = values[codepoint]
        if value != current:
            ranges.append((start, codepoint - 1, current))
            start = codepoint
            current = value
    ranges.append((start, MAX_CODEPOINT, current))
    return ranges


def data_lines(path: Path):
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        content = raw_line.split("#", 1)[0].strip()
        if content:
            yield [field.strip() for field in content.split(";")]


def parse_brackets(path: Path) -> list[tuple[int, int, str]]:
    values: list[tuple[int, int, str]] = []
    for fields in data_lines(path):
        if len(fields) < 3:
            raise ValueError(f"invalid BidiBrackets row: {fields}")
        codepoint = int(fields[0], 16)
        paired = int(fields[1], 16)
        bracket_type = fields[2]
        if codepoint > MAX_CODEPOINT or paired > MAX_CODEPOINT or bracket_type not in {"o", "c", "n"}:
            raise ValueError(f"invalid BidiBrackets row: {fields}")
        values.append((codepoint, paired, bracket_type))
    values.sort()
    if len({item[0] for item in values}) != len(values):
        raise ValueError("duplicate BidiBrackets codepoint")
    return values


def parse_mirroring(path: Path) -> list[tuple[int, int]]:
    values: list[tuple[int, int]] = []
    for fields in data_lines(path):
        if len(fields) < 2:
            raise ValueError(f"invalid BidiMirroring row: {fields}")
        codepoint = int(fields[0], 16)
        mirror = int(fields[1], 16)
        if codepoint > MAX_CODEPOINT or mirror > MAX_CODEPOINT:
            raise ValueError(f"invalid BidiMirroring row: {fields}")
        values.append((codepoint, mirror))
    values.sort()
    if len({item[0] for item in values}) != len(values):
        raise ValueError("duplicate BidiMirroring codepoint")
    return values


def cpp_string(value: str) -> str:
    return json.dumps(value, ensure_ascii=True)


def render_array(name: str, type_name: str, rows: list[str]) -> str:
    body = "\n".join(f"    {row}," for row in rows)
    return (
        f"inline constexpr std::array<{type_name}, {len(rows)}> {name}{{{{\n"
        f"{body}\n"
        "}};\n"
    )


def generate(
    derived_path: Path,
    brackets_path: Path,
    mirroring_path: Path,
    aliases_path: Path,
) -> tuple[str, dict[str, object]]:
    aliases, long_names = parse_aliases(aliases_path)
    ranges = parse_derived_bidi(derived_path, aliases)
    brackets = parse_brackets(brackets_path)
    mirrors = parse_mirroring(mirroring_path)

    canonical = {
        "version": UNICODE_VERSION,
        "classes": BIDI_CLASS_ORDER,
        "ranges": ranges,
        "brackets": brackets,
        "mirrors": mirrors,
    }
    fingerprint = hashlib.sha256(
        json.dumps(canonical, separators=(",", ":"), sort_keys=True).encode("utf-8")
    ).hexdigest()
    source_hashes = {
        "DerivedBidiClass.txt": sha256(derived_path),
        "BidiBrackets.txt": sha256(brackets_path),
        "BidiMirroring.txt": sha256(mirroring_path),
        "PropertyValueAliases.txt": sha256(aliases_path),
    }

    class_rows = [
        f"BidiClassRange{{0x{start:X}U, 0x{end:X}U, BidiClass::{value}}}"
        for start, end, value in ranges
    ]
    type_name = {"o": "Open", "c": "Close", "n": "None"}
    bracket_rows = [
        f"BidiBracketEntry{{0x{codepoint:X}U, 0x{paired:X}U, BidiBracketType::{type_name[kind]}}}"
        for codepoint, paired, kind in brackets
    ]
    mirror_rows = [
        f"BidiMirrorEntry{{0x{codepoint:X}U, 0x{mirror:X}U}}"
        for codepoint, mirror in mirrors
    ]
    short_rows = [cpp_string(value) for value in BIDI_CLASS_ORDER]
    long_rows = [cpp_string(long_names[value]) for value in BIDI_CLASS_ORDER]

    header = """#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace zevryon::text {

"""
    header += f"inline constexpr std::string_view kUnicodeBidiDataVersion = {cpp_string(UNICODE_VERSION)};\n"
    header += f"inline constexpr std::string_view kUnicodeBidiDataFingerprint = {cpp_string(fingerprint)};\n"
    for filename, digest in source_hashes.items():
        constant = re.sub(r"[^A-Za-z0-9]", "", filename)
        header += f"inline constexpr std::string_view k{constant}Sha256 = {cpp_string(digest)};\n"
    header += "\n"
    header += "enum class BidiClass : std::uint8_t {\n"
    header += "\n".join(
        f"    {value} = {index}," for index, value in enumerate(BIDI_CLASS_ORDER)
    )
    header += f"\n    Count = {len(BIDI_CLASS_ORDER)}\n}};\n\n"
    header += "enum class BidiBracketType : std::uint8_t { None = 0, Open, Close };\n"
    header += "struct BidiClassRange { std::uint32_t start; std::uint32_t end; BidiClass value; };\n"
    header += "struct BidiBracketEntry { std::uint32_t codepoint; std::uint32_t paired; BidiBracketType type; };\n"
    header += "struct BidiMirrorEntry { std::uint32_t codepoint; std::uint32_t mirror; };\n\n"
    header += render_array("kBidiClassShortNames", "std::string_view", short_rows)
    header += render_array("kBidiClassLongNames", "std::string_view", long_rows)
    header += render_array("kBidiClassRanges", "BidiClassRange", class_rows)
    header += render_array("kBidiBracketEntries", "BidiBracketEntry", bracket_rows)
    header += render_array("kBidiMirrorEntries", "BidiMirrorEntry", mirror_rows)
    header += "\nstatic_assert(kBidiClassShortNames.size() == static_cast<std::size_t>(BidiClass::Count));\n"
    header += "static_assert(kBidiClassLongNames.size() == static_cast<std::size_t>(BidiClass::Count));\n"
    header += "\n} // namespace zevryon::text\n"

    manifest: dict[str, object] = {
        "schema": "zevryon.unicode-bidi-data.v1",
        "unicode_version": UNICODE_VERSION,
        "fingerprint": fingerprint,
        "source_sha256": source_hashes,
        "bidi_class_count": len(BIDI_CLASS_ORDER),
        "bidi_class_range_count": len(ranges),
        "bracket_entry_count": len(brackets),
        "mirror_entry_count": len(mirrors),
        "generated_header_bytes": len(header.encode("utf-8")),
    }
    return header, manifest


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--derived-bidi-class", type=Path, required=True)
    parser.add_argument("--bidi-brackets", type=Path, required=True)
    parser.add_argument("--bidi-mirroring", type=Path, required=True)
    parser.add_argument("--aliases", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    arguments = parser.parse_args()

    header, manifest = generate(
        arguments.derived_bidi_class,
        arguments.bidi_brackets,
        arguments.bidi_mirroring,
        arguments.aliases,
    )
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.manifest.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(header, encoding="utf-8", newline="\n")
    arguments.manifest.write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
        newline="\n",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
