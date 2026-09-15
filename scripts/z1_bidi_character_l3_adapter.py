#!/usr/bin/env python3
from __future__ import annotations

import argparse
import re
from pathlib import Path

RANGE_RE = re.compile(
    r"BidiClassRange\{0x([0-9A-F]+)U, 0x([0-9A-F]+)U, BidiClass::([A-Z0-9_]+)\}"
)


def parse_nsm_ranges(header: Path) -> list[tuple[int, int]]:
    ranges: list[tuple[int, int]] = []
    for match in RANGE_RE.finditer(header.read_text(encoding="utf-8")):
        if match.group(3) == "NSM":
            ranges.append((int(match.group(1), 16), int(match.group(2), 16)))
    if not ranges:
        raise RuntimeError("Unicode 17 NSM ranges were not found in generated bidi data")
    return ranges


def is_nsm(codepoint: int, ranges: list[tuple[int, int]]) -> bool:
    lo = 0
    hi = len(ranges)
    while lo < hi:
        mid = (lo + hi) // 2
        start, end = ranges[mid]
        if codepoint < start:
            hi = mid
        elif codepoint > end:
            lo = mid + 1
        else:
            return True
    return False


def apply_l3(
    codepoints: list[int], order: list[int], nsm_ranges: list[tuple[int, int]]
) -> list[int]:
    result = list(order)
    position = 0
    while position < len(result):
        first = result[position]
        if first >= len(codepoints) or not is_nsm(codepoints[first], nsm_ranges):
            position += 1
            continue

        after_marks = position + 1
        previous = first
        while after_marks < len(result):
            active = result[after_marks]
            if (
                active >= len(codepoints)
                or not is_nsm(codepoints[active], nsm_ranges)
                or active + 1 != previous
            ):
                break
            previous = active
            after_marks += 1

        if after_marks >= len(result):
            break

        base = result[after_marks]
        mark_count = after_marks - position
        if (
            base >= len(codepoints)
            or is_nsm(codepoints[base], nsm_ranges)
            or base + mark_count != first
        ):
            position += 1
            continue

        result[position : after_marks + 1] = reversed(
            result[position : after_marks + 1]
        )
        position = after_marks + 1
    return result


def transform(source: Path, destination: Path, header: Path) -> tuple[int, int]:
    ranges = parse_nsm_ranges(header)
    changed = 0
    cases = 0
    output: list[str] = []
    for raw in source.read_text(encoding="utf-8").splitlines():
        line = raw.rstrip("\r")
        stripped = line.split("#", 1)[0].strip()
        if not stripped or stripped.startswith("@"):
            output.append(line)
            continue

        fields = [field.strip() for field in stripped.split(";")]
        if len(fields) != 5:
            raise RuntimeError(f"unexpected BidiCharacterTest record: {line}")
        codepoints = [int(token, 16) for token in fields[0].split()]
        order = [int(token) for token in fields[4].split()] if fields[4] else []
        adapted = apply_l3(codepoints, order, ranges)
        cases += 1
        if adapted != order:
            changed += 1

        comment = ""
        if "#" in line:
            comment = " #" + line.split("#", 1)[1]
        fields[4] = " ".join(str(index) for index in adapted)
        output.append(";".join(fields) + comment)

    if cases == 0 or changed == 0:
        raise RuntimeError("L3 adapter transformed no normative cases")
    destination.write_text("\n".join(output) + "\n", encoding="utf-8")
    return cases, changed


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Adapt Unicode BidiCharacterTest's normative through-L2 reorder field "
            "to Zevryon's production L1-L3 visual-order surface without filtering cases"
        )
    )
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument(
        "--bidi-data",
        type=Path,
        default=Path("src/unicode_bidi_data.generated.hpp"),
    )
    args = parser.parse_args()
    cases, changed = transform(args.source, args.destination, args.bidi_data)
    print(f"cases={cases} l3_adjusted_cases={changed}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
