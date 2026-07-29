#!/usr/bin/env python3
from __future__ import annotations

import argparse
import pathlib


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    parser.add_argument("symbol")
    args = parser.parse_args()

    payload = args.input.read_bytes()
    if not payload:
        raise SystemExit("binary payload must be non-empty")
    if not args.symbol.isidentifier():
        raise SystemExit("symbol must be a valid C++ identifier")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        "#pragma once",
        "",
        "#include <array>",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
        "namespace zevryon::text::detail {",
        f"inline constexpr std::array<std::uint8_t, {len(payload)}U>",
        f"{args.symbol}{{{{",
    ]
    for start in range(0, len(payload), 12):
        chunk = payload[start : start + 12]
        lines.append("    " + ", ".join(f"0x{value:02X}U" for value in chunk) + ",")
    lines.extend(["}};", "", "} // namespace zevryon::text::detail", ""])
    args.output.write_text("\n".join(lines), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
