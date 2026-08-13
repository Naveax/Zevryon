#!/usr/bin/env python3
from __future__ import annotations

import argparse
from pathlib import Path
import sys

ACTIVE_ROOTS = ("src", "tests", "cmake", ".github/workflows")
LEGACY_FAIL_CLOSED_SHIM = Path("cmake/coretext_discovery.cmake")
FORBIDDEN_SUFFIXES = {".m", ".mm"}
FORBIDDEN_PATH_TOKENS = ("metal", "cocoa", "coretext")
FORBIDDEN_BUILD_FRAGMENTS = (
    "enable_language(objcxx)",
    "languages cxx objcxx",
    "find_library(metal",
    "find_library(coretext",
    "find_library(cocoa",
    "framework metal",
    "framework coretext",
    "framework cocoa",
    "native_gpu_sdk_execution_metal",
    "native_shader_execution_metal",
    "native_window_swapchain_metal",
    "native_metal_window",
    "runs-on: macos",
    "runs-on: [macos",
)
FORBIDDEN_ACTIVE_SOURCE_FRAGMENTS = (
    "defined(__apple__)",
    "zevryon_has_metal_",
)
TEXT_SUFFIXES = {
    ".c",
    ".cc",
    ".cpp",
    ".cxx",
    ".h",
    ".hh",
    ".hpp",
    ".hxx",
    ".cmake",
    ".yml",
    ".yaml",
}


def is_under(path: Path, root: str) -> bool:
    parts = path.parts
    root_parts = Path(root).parts
    return parts[: len(root_parts)] == root_parts


def active_path(path: Path) -> bool:
    return any(is_under(path, root) for root in ACTIVE_ROOTS)


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Fail if removed Apple/macOS concrete backends re-enter Zevryon"
    )
    parser.add_argument("root", nargs="?", default=".")
    args = parser.parse_args()

    root = Path(args.root).resolve()
    failures: list[str] = []

    shim = root / LEGACY_FAIL_CLOSED_SHIM
    if not shim.is_file():
        failures.append(
            f"missing fail-closed legacy shim: {LEGACY_FAIL_CLOSED_SHIM.as_posix()}"
        )
    else:
        shim_text = read_text(shim)
        shim_lower = shim_text.lower()
        if "if(apple)" not in shim_lower or "message(fatal_error" not in shim_lower:
            failures.append(
                "legacy CoreText include point must fail closed on APPLE"
            )
        for forbidden in (
            "add_library(",
            "add_executable(",
            "target_sources(",
            "find_library(",
            "enable_language(",
        ):
            if forbidden in shim_lower:
                failures.append(
                    f"legacy CoreText shim contains executable build logic: {forbidden}"
                )

    for path in sorted(root.rglob("*")):
        if not path.is_file():
            continue
        try:
            rel = path.relative_to(root)
        except ValueError:
            continue
        if not active_path(rel):
            continue

        rel_lower = rel.as_posix().lower()
        if rel == LEGACY_FAIL_CLOSED_SHIM:
            continue

        if path.suffix.lower() in FORBIDDEN_SUFFIXES:
            failures.append(f"Objective-C source is forbidden: {rel.as_posix()}")

        if any(token in rel_lower for token in FORBIDDEN_PATH_TOKENS):
            failures.append(f"Apple backend path is forbidden: {rel.as_posix()}")

        is_text_source = path.suffix.lower() in TEXT_SUFFIXES or path.name == "CMakeLists.txt"
        if is_text_source:
            text_lower = read_text(path).lower()
            for fragment in FORBIDDEN_ACTIVE_SOURCE_FRAGMENTS:
                if fragment in text_lower:
                    failures.append(
                        f"forbidden Apple compile fragment {fragment!r} in {rel.as_posix()}"
                    )

            if path.suffix.lower() in {".cmake", ".yml", ".yaml"} or path.name == "CMakeLists.txt":
                for fragment in FORBIDDEN_BUILD_FRAGMENTS:
                    if fragment in text_lower:
                        failures.append(
                            f"forbidden Apple build fragment {fragment!r} in {rel.as_posix()}"
                        )

    if failures:
        print("Apple backend removal guard: FAIL", file=sys.stderr)
        for failure in failures:
            print(f" - {failure}", file=sys.stderr)
        return 1

    print("Apple backend removal guard: PASS")
    print("concrete macOS/Metal/CoreText/Cocoa build surface absent")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
