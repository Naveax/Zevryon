#!/usr/bin/env python3
"""Generate exact Unicode 17 search-normalization authority evidence.

Downloads the pinned UCD 17.0.0 inputs from unicode.org, records SHA-256 and
byte lengths, invokes the repository generator, and preserves NormalizationTest
for the subsequent C++ conformance admission.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
import sys
import urllib.request
from pathlib import Path
from urllib.parse import urljoin

UNICODE_VERSION = "17.0.0"
DEFAULT_BASE = f"https://www.unicode.org/Public/{UNICODE_VERSION}/ucd/"
FILES = (
    "UnicodeData.txt",
    "DerivedNormalizationProps.txt",
    "CaseFolding.txt",
    "NormalizationTest.txt",
)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def fetch(url: str, output: Path) -> None:
    request = urllib.request.Request(url, headers={"User-Agent": "Zevryon-M4-Unicode-Authority/1"})
    with urllib.request.urlopen(request, timeout=60) as response, output.open("wb") as handle:
        while True:
            block = response.read(1 << 20)
            if not block:
                break
            handle.write(block)


def verify_header(path: Path, expected: str) -> None:
    first_lines = path.read_text(encoding="utf-8").splitlines()[:20]
    if not any(line.strip() == expected for line in first_lines):
        raise RuntimeError(f"{path.name} missing expected header: {expected}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--source-base", default=DEFAULT_BASE)
    parser.add_argument(
        "--generator",
        default="scripts/generate_unicode_search_normalization.py",
    )
    parser.add_argument("--head-sha", default="")
    args = parser.parse_args()

    output = Path(args.output_dir)
    sources = output / "sources"
    output.mkdir(parents=True, exist_ok=True)
    sources.mkdir(parents=True, exist_ok=True)

    manifest_sources: dict[str, dict[str, object]] = {}
    for name in FILES:
        url = urljoin(args.source_base.rstrip("/") + "/", name)
        destination = sources / name
        fetch(url, destination)
        manifest_sources[name] = {
            "url": url,
            "sha256": sha256(destination),
            "bytes": destination.stat().st_size,
        }

    verify_header(
        sources / "DerivedNormalizationProps.txt",
        f"# DerivedNormalizationProps-{UNICODE_VERSION}.txt",
    )
    verify_header(
        sources / "CaseFolding.txt",
        f"# CaseFolding-{UNICODE_VERSION}.txt",
    )
    verify_header(
        sources / "NormalizationTest.txt",
        f"# NormalizationTest-{UNICODE_VERSION}.txt",
    )

    generated_header = output / "unicode_search_normalization_data.generated.hpp"
    metadata = output / "unicode_search_normalization_metadata.json"
    subprocess.run(
        [
            sys.executable,
            args.generator,
            "--unicode-data",
            str(sources / "UnicodeData.txt"),
            "--derived-normalization-props",
            str(sources / "DerivedNormalizationProps.txt"),
            "--case-folding",
            str(sources / "CaseFolding.txt"),
            "--unicode-version",
            UNICODE_VERSION,
            "--output",
            str(generated_header),
            "--metadata-output",
            str(metadata),
        ],
        check=True,
    )

    generated_metadata = json.loads(metadata.read_text(encoding="utf-8"))
    expected_hashes = {
        name: str(manifest_sources[name]["sha256"])
        for name in ("UnicodeData.txt", "DerivedNormalizationProps.txt", "CaseFolding.txt")
    }
    if generated_metadata.get("unicode_version") != UNICODE_VERSION:
        raise RuntimeError("generated metadata Unicode version mismatch")
    if generated_metadata.get("source_sha256") != expected_hashes:
        raise RuntimeError("generated metadata source SHA mismatch")

    shutil.copy2(sources / "NormalizationTest.txt", output / "NormalizationTest.txt")
    manifest = {
        "schema": "zevryon.m4.unicode-search-authority.v1",
        "unicode_version": UNICODE_VERSION,
        "head_sha": args.head_sha,
        "source_base": args.source_base,
        "sources": manifest_sources,
        "generator": {
            "path": args.generator,
            "sha256": sha256(Path(args.generator)),
        },
        "generated": {
            "header": generated_header.name,
            "header_sha256": sha256(generated_header),
            "metadata": metadata.name,
            "metadata_sha256": sha256(metadata),
            "normalization_test": "NormalizationTest.txt",
            "normalization_test_sha256": sha256(output / "NormalizationTest.txt"),
        },
    }
    (output / "authority.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
        newline="\n",
    )
    print(json.dumps(manifest, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
