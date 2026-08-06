#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys

from z2r3d_codec_certification import (
    CertificationError,
    build_manifest,
    canonical_bytes,
    load_json,
)

MANIFEST_SCHEMA = "zevryon.z2r3e.platform-authority-certification.v1"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--platform", choices=("linux", "windows", "macos"), required=True)
    parser.add_argument("--commit-sha", required=True)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--build-type", default="Release")
    parser.add_argument("--max-p50-ratio", type=float, default=2.00)
    parser.add_argument("--max-p95-ratio", type=float, default=2.25)
    parser.add_argument("--max-p99-ratio", type=float, default=2.50)
    parser.add_argument("--max-maximum-ratio", type=float, default=3.00)
    parser.add_argument("--max-wall-delta-seconds", type=float, default=5.0)
    parser.add_argument("--max-memory-ratio", type=float, default=1.50)
    parser.add_argument("--max-memory-delta-bytes", type=int, default=16 * 1024 * 1024)
    parser.add_argument("--max-total-ratio", type=float, default=2.00)
    parser.add_argument("--max-total-delta-seconds", type=float, default=10.0)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    try:
        manifest = build_manifest(
            load_json(args.report),
            platform_name=args.platform,
            commit_sha=args.commit_sha,
            compiler=args.compiler,
            build_type=args.build_type,
            max_p50_ratio=args.max_p50_ratio,
            max_p95_ratio=args.max_p95_ratio,
            max_p99_ratio=args.max_p99_ratio,
            max_maximum_ratio=args.max_maximum_ratio,
            max_wall_delta_seconds=args.max_wall_delta_seconds,
            max_memory_ratio=args.max_memory_ratio,
            max_memory_delta_bytes=args.max_memory_delta_bytes,
            max_total_ratio=args.max_total_ratio,
            max_total_delta_seconds=args.max_total_delta_seconds,
        )
    except CertificationError as error:
        print(f"Z2R-3E platform authority certification failed: {error}", file=sys.stderr)
        return 1

    manifest.pop("manifest_sha256", None)
    manifest["schema"] = MANIFEST_SCHEMA
    manifest["slice"] = "Z2R-3E-massivedoc-codec-authority"
    manifest["authority"] = {
        "authoritative_backend": "rust",
        "reverse_shadow_backend": "cpp",
        "authoritative_switch_performed": True,
        "fallback_permitted": False,
        "rollback": [
            "ZEVRYON_RUST_MASSIVEDOC_CODEC_AUTHORITATIVE=OFF",
            "ZEVRYON_RUST_MASSIVEDOC_CODEC_SHADOW=OFF",
            "ZEVRYON_ENABLE_RUST_CORE=OFF",
        ],
    }
    manifest["authority_ready"] = True
    manifest["promotion_ready"] = False
    manifest["manifest_sha256"] = hashlib.sha256(canonical_bytes(manifest)).hexdigest()

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(
        json.dumps(
            {
                "schema": manifest["schema"],
                "platform": manifest["platform"],
                "authority_ready": manifest["authority_ready"],
                "manifest_sha256": manifest["manifest_sha256"],
            },
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
