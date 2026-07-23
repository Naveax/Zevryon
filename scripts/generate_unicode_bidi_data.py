#!/usr/bin/env python3
"""Generate compact, deterministic Unicode Bidi_Class tables."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

UNICODE_VERSION = "17.0.0"
MAX_CODEPOINT = 0x10FFFF
BIDI_ORDER = (
    "L", "R", "AL", "EN", "ES", "ET", "AN", "CS", "NSM", "BN",
    "B", "S", "WS", "ON", "LRE", "LRO", "RLE", "RLO", "PDF",
    "LRI", "RLI", "FSI", "PDI",
)


@dataclass(frozen=True, order=True)
class RangeValue:
    start: int
    end: int
    value: str


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
    alias_to_short: dict[str, str] = {}
    short_to_long: dict[str, str] = {}
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        content = raw_line.split("#", 1)[0].strip()
        if not content:
            continue
        fields = [field.strip() for field in content.split(";")]
        if len(fields) < 3 or fields[0] != "bc":
            continue
        short_name, long_name = fields[1], fields[2]
        alias_to_short[short_name] = short_name
        alias_to_short[long_name] = short_name
        short_to_long[short_name] = long_name
        for alias in fields[3:]:
            if alias and alias != "n/a":
                alias_to_short[alias] = short_name
    missing = [name for name in BIDI_ORDER if name not in short_to_long]
    if missing:
        raise ValueError(f"missing Bidi_Class aliases: {missing}")
    return alias_to_short, short_to_long


def normalize(name: str, aliases: dict[str, str]) -> str:
    try:
        return aliases[name]
    except KeyError as exc:
        raise ValueError(f"unknown Bidi_Class value: {name}") from exc


def data_lines(path: Path) -> Iterable[tuple[str, str]]:
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        content = raw_line.split("#", 1)[0].strip()
        if not content:
            continue
        left, separator, right = content.partition(";")
        if not separator:
            raise ValueError(f"invalid UCD line in {path}: {raw_line}")
        yield left.strip(), right.strip()


def missing_lines(path: Path) -> Iterable[tuple[str, str]]:
    pattern = re.compile(r"^\s*#\s*@missing:\s*([^;]+);\s*([^#\s]+)")
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        match = pattern.match(raw_line)
        if match:
            yield match.group(1).strip(), match.group(2).strip()


def parse_bidi_classes(path: Path, aliases: dict[str, str]) -> list[RangeValue]:
    values = ["L"] * (MAX_CODEPOINT + 1)
    for token, value in missing_lines(path):
        start, end = parse_range(token)
        normalized = normalize(value, aliases)
        values[start : end + 1] = [normalized] * (end - start + 1)
    for token, value in data_lines(path):
        start, end = parse_range(token)
        normalized = normalize(value, aliases)
        values[start : end + 1] = [normalized] * (end - start + 1)

    ranges: list[RangeValue] = []
    start = 0
    current = values[0]
    for codepoint in range(1, MAX_CODEPOINT + 1):
        if values[codepoint] == current:
            continue
        ranges.append(RangeValue(start, codepoint - 1, current))
        start = codepoint
        current = values[codepoint]
    ranges.append(RangeValue(start, MAX_CODEPOINT, current))
    return ranges


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
    derived_bidi_path: Path,
    aliases_path: Path,
) -> tuple[str, dict[str, object]]:
    aliases, short_to_long = parse_aliases(aliases_path)
    ranges = parse_bidi_classes(derived_bidi_path, aliases)
    ids = {name: index for index, name in enumerate(BIDI_ORDER)}

    canonical = {
        "version": UNICODE_VERSION,
        "classes": [[item.start, item.end, item.value] for item in ranges],
        "aliases": [[name, short_to_long[name]] for name in BIDI_ORDER],
    }
    fingerprint = hashlib.sha256(
        json.dumps(canonical, separators=(",", ":"), sort_keys=True).encode("utf-8")
    ).hexdigest()
    source_hashes = {
        "DerivedBidiClass.txt": sha256(derived_bidi_path),
        "PropertyValueAliases.txt": sha256(aliases_path),
    }

    enum_rows = [f"{name} = {ids[name]}" for name in BIDI_ORDER]
    short_rows = [cpp_string(name) for name in BIDI_ORDER]
    long_rows = [cpp_string(short_to_long[name]) for name in BIDI_ORDER]
    range_rows = [
        f"BidiClassRange{{0x{item.start:X}U, 0x{item.end:X}U, BidiClass::{item.value}}}"
        for item in ranges
    ]

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
    header += "\n".join(f"    {row}," for row in enum_rows)
    header += f"\n    Count = {len(BIDI_ORDER)}\n}};\n\n"
    header += "struct BidiClassRange { std::uint32_t start; std::uint32_t end; BidiClass value; };\n\n"
    header += render_array("kBidiClassShortNames", "std::string_view", short_rows)
    header += render_array("kBidiClassLongNames", "std::string_view", long_rows)
    header += render_array("kBidiClassRanges", "BidiClassRange", range_rows)
    header += "\nstatic_assert(kBidiClassShortNames.size() == static_cast<std::size_t>(BidiClass::Count));\n"
    header += "static_assert(kBidiClassLongNames.size() == static_cast<std::size_t>(BidiClass::Count));\n"
    header += "\n} // namespace zevryon::text\n"

    manifest: dict[str, object] = {
        "schema": "zevryon.unicode-bidi-data.v1",
        "unicode_version": UNICODE_VERSION,
        "fingerprint": fingerprint,
        "source_sha256": source_hashes,
        "bidi_class_count": len(BIDI_ORDER),
        "bidi_range_count": len(ranges),
        "generated_header_bytes": len(header.encode("utf-8")),
    }
    return header, manifest


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--derived-bidi-class", type=Path, required=True)
    parser.add_argument("--aliases", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    arguments = parser.parse_args()

    header, manifest = generate(arguments.derived_bidi_class, arguments.aliases)
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
