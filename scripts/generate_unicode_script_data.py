#!/usr/bin/env python3
"""Generate compact, deterministic Unicode Script/Script_Extensions tables."""

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


def data_lines(path: Path) -> Iterable[tuple[str, str]]:
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        content = raw_line.split("#", 1)[0].strip()
        if not content:
            continue
        left, separator, right = content.partition(";")
        if not separator:
            raise ValueError(f"invalid UCD line in {path}: {raw_line}")
        yield left.strip(), right.strip()


def parse_aliases(path: Path) -> tuple[dict[str, str], dict[str, str]]:
    long_to_short: dict[str, str] = {}
    short_to_long: dict[str, str] = {}
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        content = raw_line.split("#", 1)[0].strip()
        if not content:
            continue
        fields = [field.strip() for field in content.split(";")]
        if len(fields) < 3 or fields[0] != "sc":
            continue
        short_name, long_name = fields[1], fields[2]
        if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_]*", short_name):
            raise ValueError(f"invalid Script alias: {short_name}")
        long_to_short[long_name] = short_name
        short_to_long[short_name] = long_name
        for alias in fields[3:]:
            if alias and alias != "n/a":
                long_to_short[alias] = short_name
    if "Zzzz" not in short_to_long or "Zyyy" not in short_to_long or "Zinh" not in short_to_long:
        raise ValueError("required Unknown/Common/Inherited Script aliases missing")
    return long_to_short, short_to_long


def normalize_script(name: str, aliases: dict[str, str]) -> str:
    if name in aliases.values():
        return name
    try:
        return aliases[name]
    except KeyError as exc:
        raise ValueError(f"unknown Script value: {name}") from exc


def merge_ranges(values: list[RangeValue]) -> list[RangeValue]:
    values.sort(key=lambda item: (item.start, item.end, item.value))
    merged: list[RangeValue] = []
    for item in values:
        if merged and item.start <= merged[-1].end:
            raise ValueError(
                f"overlapping ranges: U+{merged[-1].start:04X}..U+{merged[-1].end:04X} "
                f"and U+{item.start:04X}..U+{item.end:04X}"
            )
        if merged and merged[-1].value == item.value and merged[-1].end + 1 == item.start:
            previous = merged[-1]
            merged[-1] = RangeValue(previous.start, item.end, item.value)
        else:
            merged.append(item)
    return merged


def parse_scripts(path: Path, aliases: dict[str, str]) -> list[RangeValue]:
    ranges: list[RangeValue] = []
    for token, value in data_lines(path):
        start, end = parse_range(token)
        ranges.append(RangeValue(start, end, normalize_script(value, aliases)))
    return merge_ranges(ranges)


def parse_script_extensions(
    path: Path,
    aliases: dict[str, str],
) -> list[tuple[int, int, tuple[str, ...]]]:
    ranges: list[tuple[int, int, tuple[str, ...]]] = []
    for token, value in data_lines(path):
        start, end = parse_range(token)
        scripts = tuple(sorted({normalize_script(item, aliases) for item in value.split()}))
        if not scripts:
            raise ValueError(f"empty Script_Extensions set for {token}")
        ranges.append((start, end, scripts))
    ranges.sort(key=lambda item: (item[0], item[1], item[2]))
    merged: list[tuple[int, int, tuple[str, ...]]] = []
    for item in ranges:
        if merged and item[0] <= merged[-1][1]:
            raise ValueError("overlapping Script_Extensions ranges")
        if merged and merged[-1][2] == item[2] and merged[-1][1] + 1 == item[0]:
            merged[-1] = (merged[-1][0], item[1], item[2])
        else:
            merged.append(item)
    return merged


def enum_order(short_to_long: dict[str, str]) -> list[str]:
    scripts = sorted(short_to_long)
    scripts.remove("Zzzz")
    return ["Zzzz", *scripts]


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
    scripts_path: Path,
    extensions_path: Path,
    aliases_path: Path,
) -> tuple[str, dict[str, object]]:
    long_to_short, short_to_long = parse_aliases(aliases_path)
    scripts = parse_scripts(scripts_path, long_to_short)
    extensions = parse_script_extensions(extensions_path, long_to_short)
    order = enum_order(short_to_long)
    ids = {name: index for index, name in enumerate(order)}
    if len(order) > 0xFFFF:
        raise ValueError("Script enum exceeds uint16_t")

    unique_sets = sorted({item[2] for item in extensions})
    set_index = {value: index for index, value in enumerate(unique_sets)}
    extension_pool: list[str] = []
    set_records: list[tuple[int, int]] = []
    for values in unique_sets:
        offset = len(extension_pool)
        extension_pool.extend(values)
        set_records.append((offset, len(values)))

    canonical = {
        "version": UNICODE_VERSION,
        "scripts": [[item.start, item.end, item.value] for item in scripts],
        "extensions": [[start, end, list(values)] for start, end, values in extensions],
        "aliases": [[name, short_to_long[name]] for name in order],
    }
    fingerprint = hashlib.sha256(
        json.dumps(canonical, separators=(",", ":"), sort_keys=True).encode("utf-8")
    ).hexdigest()
    source_hashes = {
        "Scripts.txt": sha256(scripts_path),
        "ScriptExtensions.txt": sha256(extensions_path),
        "PropertyValueAliases.txt": sha256(aliases_path),
    }

    enum_rows = [f"{name} = {ids[name]}" for name in order]
    short_rows = [cpp_string(name) for name in order]
    long_rows = [cpp_string(short_to_long[name]) for name in order]
    script_rows = [
        f"ScriptRange{{0x{item.start:X}U, 0x{item.end:X}U, ScriptId::{item.value}}}"
        for item in scripts
    ]
    pool_rows = [f"ScriptId::{name}" for name in extension_pool]
    set_rows = [f"ScriptSetRecord{{{offset}U, {count}U}}" for offset, count in set_records]
    extension_rows = [
        f"ScriptExtensionRange{{0x{start:X}U, 0x{end:X}U, {set_index[values]}U}}"
        for start, end, values in extensions
    ]

    header = """#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace zevryon::text {

"""
    header += f"inline constexpr std::string_view kUnicodeScriptDataVersion = {cpp_string(UNICODE_VERSION)};\n"
    header += f"inline constexpr std::string_view kUnicodeScriptDataFingerprint = {cpp_string(fingerprint)};\n"
    for filename, digest in source_hashes.items():
        constant = re.sub(r"[^A-Za-z0-9]", "", filename)
        header += f"inline constexpr std::string_view k{constant}Sha256 = {cpp_string(digest)};\n"
    header += "\n"
    header += "enum class ScriptId : std::uint16_t {\n"
    header += "\n".join(f"    {row}," for row in enum_rows)
    header += f"\n    Count = {len(order)}\n}};\n\n"
    header += "struct ScriptRange { std::uint32_t start; std::uint32_t end; ScriptId script; };\n"
    header += "struct ScriptSetRecord { std::uint32_t offset; std::uint16_t count; };\n"
    header += "struct ScriptExtensionRange { std::uint32_t start; std::uint32_t end; std::uint16_t set_index; };\n\n"
    header += render_array("kScriptShortNames", "std::string_view", short_rows)
    header += render_array("kScriptLongNames", "std::string_view", long_rows)
    header += render_array("kScriptRanges", "ScriptRange", script_rows)
    header += render_array("kScriptExtensionPool", "ScriptId", pool_rows)
    header += render_array("kScriptExtensionSets", "ScriptSetRecord", set_rows)
    header += render_array("kScriptExtensionRanges", "ScriptExtensionRange", extension_rows)
    header += "\nstatic_assert(kScriptShortNames.size() == static_cast<std::size_t>(ScriptId::Count));\n"
    header += "static_assert(kScriptLongNames.size() == static_cast<std::size_t>(ScriptId::Count));\n"
    header += "\n} // namespace zevryon::text\n"

    manifest: dict[str, object] = {
        "schema": "zevryon.unicode-script-data.v1",
        "unicode_version": UNICODE_VERSION,
        "fingerprint": fingerprint,
        "source_sha256": source_hashes,
        "script_count": len(order),
        "script_range_count": len(scripts),
        "script_extension_range_count": len(extensions),
        "script_extension_set_count": len(unique_sets),
        "script_extension_pool_count": len(extension_pool),
        "generated_header_bytes": len(header.encode("utf-8")),
    }
    return header, manifest


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--scripts", type=Path, required=True)
    parser.add_argument("--script-extensions", type=Path, required=True)
    parser.add_argument("--aliases", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    arguments = parser.parse_args()

    header, manifest = generate(
        arguments.scripts,
        arguments.script_extensions,
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
