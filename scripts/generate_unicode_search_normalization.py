#!/usr/bin/env python3
"""Generate deterministic Unicode search-normalization tables.

The generated tables implement Q(X) = NFC(toCasefold(NFD(X))) using the
Unicode Standard's default full case folding. Source inputs are the UCD
UnicodeData.txt, DerivedNormalizationProps.txt, and CaseFolding.txt files.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

UNICODE_VERSION = "17.0.0"
MAX_CODEPOINT = 0x10FFFF
HANGUL_S_BASE = 0xAC00
HANGUL_S_COUNT = 11172


@dataclass(frozen=True)
class Tables:
    decompositions: tuple[tuple[int, tuple[int, ...]], ...]
    case_folds: tuple[tuple[int, tuple[int, ...]], ...]
    combining_ranges: tuple[tuple[int, int, int], ...]
    compositions: tuple[tuple[int, int, int], ...]


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def stripped_lines(path: Path) -> Iterable[str]:
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.split("#", 1)[0].strip()
        if line:
            yield line


def verify_version_header(path: Path, stem: str, version: str) -> None:
    expected = f"# {stem}-{version}.txt"
    first = path.read_text(encoding="utf-8").splitlines()[:20]
    if not any(line.strip() == expected for line in first):
        raise ValueError(f"{path} does not declare {expected}")


def parse_unicode_data(path: Path) -> tuple[dict[int, int], dict[int, tuple[int, ...]]]:
    combining: dict[int, int] = {}
    direct_decomp: dict[int, tuple[int, ...]] = {}
    pending_first: tuple[int, int] | None = None

    for raw in path.read_text(encoding="utf-8").splitlines():
        if not raw:
            continue
        fields = raw.split(";")
        if len(fields) < 6:
            raise ValueError(f"invalid UnicodeData row: {raw}")
        cp = int(fields[0], 16)
        if cp > MAX_CODEPOINT:
            raise ValueError(f"code point out of range: {fields[0]}")
        name = fields[1]
        ccc = int(fields[3])
        if not 0 <= ccc <= 255:
            raise ValueError(f"invalid combining class for U+{cp:04X}")

        if name.endswith(", First>"):
            pending_first = (cp, ccc)
            continue
        if name.endswith(", Last>"):
            if pending_first is None or pending_first[0] > cp:
                raise ValueError("malformed UnicodeData First/Last range")
            if pending_first[1] != ccc:
                raise ValueError("UnicodeData range combining class mismatch")
            if ccc != 0:
                for value in range(pending_first[0], cp + 1):
                    combining[value] = ccc
            pending_first = None
            continue
        if pending_first is not None:
            raise ValueError("unterminated UnicodeData First/Last range")

        if ccc != 0:
            combining[cp] = ccc

        decomposition = fields[5].strip()
        if decomposition and not decomposition.startswith("<"):
            values = tuple(int(token, 16) for token in decomposition.split())
            if not values or any(value > MAX_CODEPOINT for value in values):
                raise ValueError(f"invalid canonical decomposition for U+{cp:04X}")
            direct_decomp[cp] = values

    if pending_first is not None:
        raise ValueError("unterminated UnicodeData range at EOF")
    return combining, direct_decomp


def parse_full_composition_exclusions(path: Path, version: str) -> set[int]:
    verify_version_header(path, "DerivedNormalizationProps", version)
    excluded: set[int] = set()
    for line in stripped_lines(path):
        left, separator, right = line.partition(";")
        if not separator or right.strip() != "Full_Composition_Exclusion":
            continue
        token = left.strip()
        pieces = token.split("..")
        first = int(pieces[0], 16)
        last = int(pieces[-1], 16)
        if first > last or last > MAX_CODEPOINT:
            raise ValueError(f"invalid exclusion range: {token}")
        excluded.update(range(first, last + 1))
    return excluded


def parse_case_folding(path: Path, version: str) -> dict[int, tuple[int, ...]]:
    verify_version_header(path, "CaseFolding", version)
    mappings: dict[int, tuple[int, ...]] = {}
    priorities: dict[int, int] = {}
    for line in stripped_lines(path):
        fields = [field.strip() for field in line.split(";")]
        if len(fields) < 3:
            raise ValueError(f"invalid CaseFolding row: {line}")
        cp = int(fields[0], 16)
        status = fields[1]
        if status not in {"C", "F"}:
            continue
        mapping = tuple(int(token, 16) for token in fields[2].split())
        if not mapping or cp > MAX_CODEPOINT or any(value > MAX_CODEPOINT for value in mapping):
            raise ValueError(f"invalid case-fold mapping for U+{cp:04X}")
        priority = 2 if status == "F" else 1
        if priority >= priorities.get(cp, 0):
            mappings[cp] = mapping
            priorities[cp] = priority
    return mappings


def expand_canonical_decompositions(
    direct: dict[int, tuple[int, ...]],
) -> dict[int, tuple[int, ...]]:
    cache: dict[int, tuple[int, ...]] = {}
    active: set[int] = set()

    def expand(cp: int) -> tuple[int, ...]:
        if HANGUL_S_BASE <= cp < HANGUL_S_BASE + HANGUL_S_COUNT:
            return (cp,)
        if cp in cache:
            return cache[cp]
        mapping = direct.get(cp)
        if mapping is None:
            return (cp,)
        if cp in active:
            raise ValueError(f"canonical decomposition cycle at U+{cp:04X}")
        active.add(cp)
        result: list[int] = []
        for child in mapping:
            result.extend(expand(child))
        active.remove(cp)
        cache[cp] = tuple(result)
        return cache[cp]

    output: dict[int, tuple[int, ...]] = {}
    for cp in sorted(direct):
        value = expand(cp)
        if value != (cp,):
            output[cp] = value
    return output


def combining_ranges(combining: dict[int, int]) -> tuple[tuple[int, int, int], ...]:
    if not combining:
        return ()
    result: list[tuple[int, int, int]] = []
    start = previous = -1
    current_ccc = -1
    for cp, ccc in sorted(combining.items()):
        if start < 0:
            start = previous = cp
            current_ccc = ccc
            continue
        if cp == previous + 1 and ccc == current_ccc:
            previous = cp
            continue
        result.append((start, previous, current_ccc))
        start = previous = cp
        current_ccc = ccc
    result.append((start, previous, current_ccc))
    return tuple(result)


def build_tables(
    unicode_data: Path,
    derived_normalization_props: Path,
    case_folding: Path,
    version: str,
) -> Tables:
    combining, direct = parse_unicode_data(unicode_data)
    excluded = parse_full_composition_exclusions(derived_normalization_props, version)
    folds = parse_case_folding(case_folding, version)
    expanded = expand_canonical_decompositions(direct)

    compositions: list[tuple[int, int, int]] = []
    for composite, mapping in direct.items():
        if composite in excluded or len(mapping) != 2:
            continue
        if HANGUL_S_BASE <= composite < HANGUL_S_BASE + HANGUL_S_COUNT:
            continue
        compositions.append((mapping[0], mapping[1], composite))
    compositions.sort()
    for previous, current in zip(compositions, compositions[1:]):
        if previous[:2] == current[:2]:
            raise ValueError(
                f"duplicate canonical composition pair: U+{current[0]:04X} U+{current[1]:04X}"
            )

    return Tables(
        decompositions=tuple(sorted(expanded.items())),
        case_folds=tuple(sorted(folds.items())),
        combining_ranges=combining_ranges(combining),
        compositions=tuple(compositions),
    )


def intern_mappings(
    entries: tuple[tuple[int, tuple[int, ...]], ...],
) -> tuple[list[tuple[int, int, int]], list[int]]:
    pool: list[int] = []
    offsets: dict[tuple[int, ...], int] = {}
    rendered: list[tuple[int, int, int]] = []
    for cp, mapping in entries:
        offset = offsets.get(mapping)
        if offset is None:
            offset = len(pool)
            offsets[mapping] = offset
            pool.extend(mapping)
        if offset > 0xFFFFFFFF or len(mapping) > 0xFFFF:
            raise ValueError("generated mapping pool exceeds runtime field widths")
        rendered.append((cp, offset, len(mapping)))
    return rendered, pool


def render_array(type_name: str, name: str, rows: list[str]) -> str:
    body = "\n".join(f"    {row}," for row in rows)
    return (
        f"inline constexpr std::array<{type_name}, {len(rows)}> {name}{{{{\n"
        f"{body}\n"
        "}};\n"
    )


def render_header(
    tables: Tables,
    version: str,
    source_hashes: dict[str, str],
) -> tuple[str, dict[str, object]]:
    decomp_entries, decomp_pool = intern_mappings(tables.decompositions)
    fold_entries, fold_pool = intern_mappings(tables.case_folds)

    canonical = {
        "version": version,
        "decompositions": [[cp, list(mapping)] for cp, mapping in tables.decompositions],
        "case_folds": [[cp, list(mapping)] for cp, mapping in tables.case_folds],
        "combining_ranges": [list(row) for row in tables.combining_ranges],
        "compositions": [list(row) for row in tables.compositions],
        "sources": source_hashes,
        "transform": "Q(X)=NFC(toCasefold(NFD(X)))",
    }
    fingerprint = hashlib.sha256(
        json.dumps(canonical, sort_keys=True, separators=(",", ":")).encode("utf-8")
    ).hexdigest()

    chunks = [
        "#pragma once\n\n",
        "#include \"unicode_search_normalizer.hpp\"\n\n",
        "#include <array>\n#include <cstdint>\n#include <string_view>\n\n",
        "namespace zevryon::text {\n\n",
        f'inline constexpr std::string_view kUnicodeSearchNormalizationVersion = "{version}";\n',
        f'inline constexpr std::string_view kUnicodeSearchNormalizationFingerprint = "{fingerprint}";\n',
    ]
    for key, value in source_hashes.items():
        symbol = re.sub(r"[^A-Za-z0-9]", "", key)
        chunks.append(f'inline constexpr std::string_view kUnicodeSearch{symbol}Sha256 = "{value}";\n')
    chunks.append("\n")

    chunks.append(render_array(
        "std::uint32_t", "kUnicodeSearchCanonicalDecompositionPool",
        [f"0x{value:06x}U" for value in decomp_pool]))
    chunks.append("\n")
    chunks.append(render_array(
        "UnicodeSearchMapEntry", "kUnicodeSearchCanonicalDecomposition",
        [f"UnicodeSearchMapEntry{{0x{cp:06x}U, {offset}U, {length}U}}" for cp, offset, length in decomp_entries]))
    chunks.append("\n")
    chunks.append(render_array(
        "std::uint32_t", "kUnicodeSearchFullCaseFoldPool",
        [f"0x{value:06x}U" for value in fold_pool]))
    chunks.append("\n")
    chunks.append(render_array(
        "UnicodeSearchMapEntry", "kUnicodeSearchFullCaseFold",
        [f"UnicodeSearchMapEntry{{0x{cp:06x}U, {offset}U, {length}U}}" for cp, offset, length in fold_entries]))
    chunks.append("\n")
    chunks.append(render_array(
        "UnicodeSearchCombiningClassRange", "kUnicodeSearchCombiningClasses",
        [f"UnicodeSearchCombiningClassRange{{0x{first:06x}U, 0x{last:06x}U, {ccc}U}}" for first, last, ccc in tables.combining_ranges]))
    chunks.append("\n")
    chunks.append(render_array(
        "UnicodeSearchCompositionEntry", "kUnicodeSearchCompositions",
        [f"UnicodeSearchCompositionEntry{{0x{first:06x}U, 0x{second:06x}U, 0x{composite:06x}U}}" for first, second, composite in tables.compositions]))
    chunks.append("\n")
    chunks.append(
        "inline constexpr UnicodeSearchNormalizationTables kUnicodeSearchNormalizationTables{\n"
        "    kUnicodeSearchNormalizationVersion,\n"
        "    kUnicodeSearchNormalizationFingerprint,\n"
        "    kUnicodeSearchCanonicalDecomposition,\n"
        "    kUnicodeSearchCanonicalDecompositionPool,\n"
        "    kUnicodeSearchFullCaseFold,\n"
        "    kUnicodeSearchFullCaseFoldPool,\n"
        "    kUnicodeSearchCombiningClasses,\n"
        "    kUnicodeSearchCompositions,\n"
        "};\n\n"
        "} // namespace zevryon::text\n"
    )

    metadata = {
        "schema": "zevryon.unicode-search-normalization.v1",
        "unicode_version": version,
        "transform": canonical["transform"],
        "fingerprint": fingerprint,
        "source_sha256": source_hashes,
        "counts": {
            "canonical_decomposition_entries": len(decomp_entries),
            "canonical_decomposition_pool": len(decomp_pool),
            "full_case_fold_entries": len(fold_entries),
            "full_case_fold_pool": len(fold_pool),
            "combining_class_ranges": len(tables.combining_ranges),
            "composition_entries": len(tables.compositions),
        },
    }
    return "".join(chunks), metadata


def generate(args: argparse.Namespace) -> None:
    unicode_data = Path(args.unicode_data)
    derived = Path(args.derived_normalization_props)
    case_folding = Path(args.case_folding)
    tables = build_tables(unicode_data, derived, case_folding, args.unicode_version)
    hashes = {
        "UnicodeData.txt": sha256(unicode_data),
        "DerivedNormalizationProps.txt": sha256(derived),
        "CaseFolding.txt": sha256(case_folding),
    }
    header, metadata = render_header(tables, args.unicode_version, hashes)
    Path(args.output).write_text(header, encoding="utf-8", newline="\n")
    if args.metadata_output:
        Path(args.metadata_output).write_text(
            json.dumps(metadata, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
            newline="\n",
        )


def self_test() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        unicode_data = root / "UnicodeData.txt"
        derived = root / "DerivedNormalizationProps.txt"
        case_folding = root / "CaseFolding.txt"
        unicode_data.write_text(
            "0041;LATIN CAPITAL LETTER A;Lu;0;L;;;;;N;;;;0061;\n"
            "0061;LATIN SMALL LETTER A;Ll;0;L;;;;;N;;;0041;;0041\n"
            "00C5;LATIN CAPITAL LETTER A WITH RING ABOVE;Lu;0;L;0041 030A;;;;N;LATIN CAPITAL LETTER A RING;;;00E5;\n"
            "00DF;LATIN SMALL LETTER SHARP S;Ll;0;L;;;;;N;;;;\n"
            "00E5;LATIN SMALL LETTER A WITH RING ABOVE;Ll;0;L;0061 030A;;;;N;LATIN SMALL LETTER A RING;;00C5;;00C5\n"
            "030A;COMBINING RING ABOVE;Mn;230;NSM;;;;;N;NON-SPACING RING ABOVE;;;;\n",
            encoding="utf-8",
        )
        derived.write_text(
            "# DerivedNormalizationProps-17.0.0.txt\n"
            "0340 ; Full_Composition_Exclusion\n",
            encoding="utf-8",
        )
        case_folding.write_text(
            "# CaseFolding-17.0.0.txt\n"
            "0041; C; 0061; # LATIN CAPITAL LETTER A\n"
            "00C5; C; 00E5; # A RING\n"
            "00DF; F; 0073 0073; # SHARP S\n",
            encoding="utf-8",
        )
        tables = build_tables(unicode_data, derived, case_folding, UNICODE_VERSION)
        assert dict(tables.decompositions)[0x00C5] == (0x0041, 0x030A)
        assert dict(tables.case_folds)[0x00DF] == (0x0073, 0x0073)
        assert (0x0041, 0x030A, 0x00C5) in tables.compositions
        assert tables.combining_ranges == ((0x030A, 0x030A, 230),)
        header, metadata = render_header(
            tables,
            UNICODE_VERSION,
            {
                "UnicodeData.txt": sha256(unicode_data),
                "DerivedNormalizationProps.txt": sha256(derived),
                "CaseFolding.txt": sha256(case_folding),
            },
        )
        assert "kUnicodeSearchNormalizationTables" in header
        assert metadata["unicode_version"] == UNICODE_VERSION
        assert metadata["transform"] == "Q(X)=NFC(toCasefold(NFD(X)))"
    print("Zevryon Unicode search normalization generator self-test passed")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--unicode-data")
    parser.add_argument("--derived-normalization-props")
    parser.add_argument("--case-folding")
    parser.add_argument("--unicode-version", default=UNICODE_VERSION)
    parser.add_argument("--output")
    parser.add_argument("--metadata-output")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return
    required = [
        args.unicode_data,
        args.derived_normalization_props,
        args.case_folding,
        args.output,
    ]
    if any(value is None for value in required):
        parser.error("generation requires all three UCD inputs and --output")
    generate(args)


if __name__ == "__main__":
    main()
