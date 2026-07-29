#!/usr/bin/env python3
from __future__ import annotations

import argparse
import pathlib
import struct


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    args = parser.parse_args()

    payload = args.input.read_bytes()
    if len(payload) == 0 or len(payload) % 4 != 0:
        raise SystemExit("SPIR-V payload must be non-empty and 32-bit aligned")
    words = struct.unpack(f"<{len(payload) // 4}I", payload)
    if words[0] != 0x07230203:
        raise SystemExit("invalid SPIR-V magic")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        "#pragma once",
        "",
        "#include <array>",
        "#include <cstdint>",
        "",
        "namespace zevryon::text::detail {",
        f"inline constexpr std::array<std::uint32_t, {len(words)}U>",
        "kVulkanIntegerComposerSpirv{{",
    ]
    for start in range(0, len(words), 8):
        chunk = words[start : start + 8]
        lines.append("    " + ", ".join(f"0x{word:08X}U" for word in chunk) + ",")
    lines.extend(["}};", "", "} // namespace zevryon::text::detail", ""])
    args.output.write_text("\n".join(lines), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
