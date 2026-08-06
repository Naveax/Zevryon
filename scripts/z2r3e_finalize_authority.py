#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys
from typing import Any

PLATFORM_SCHEMA = "zevryon.z2r3e.platform-authority-certification.v1"
FINAL_SCHEMA = "zevryon.z2r3e.authority-readiness.v1"
PLATFORMS = ("linux", "windows", "macos")
OPERATIONS = ("import", "open", "verify", "export")
EXPECTED_Z2R3D_HEAD = "acb7967ce905ba43215e09716e414584d7c790da"
EXPECTED_Z2R3D_MANIFEST = (
    "8ec0438fd621fa9770439d3b03d19cf48c05eeb84eae33cc1a4ec548e75d6a05"
)


class FinalizationError(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise FinalizationError(message)


def canonical_bytes(value: Any) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode(
        "utf-8"
    )


def load_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise FinalizationError(f"cannot load {path}: {error}") from error
    require(isinstance(value, dict), f"{path} is not a JSON object")
    return value


def validate_platform(
    manifest: dict[str, Any], platform: str, commit_sha: str
) -> dict[str, Any]:
    require(manifest.get("schema") == PLATFORM_SCHEMA, f"{platform} schema mismatch")
    require(manifest.get("platform") == platform, f"{platform} identity mismatch")
    require(manifest.get("commit_sha") == commit_sha, f"{platform} commit mismatch")
    require(manifest.get("slice_ready") is True, f"{platform} slice not ready")
    require(manifest.get("authority_ready") is True, f"{platform} authority not ready")

    claimed_sha = str(manifest.get("manifest_sha256", ""))
    require(len(claimed_sha) == 64, f"{platform} manifest SHA invalid")
    unhashed = dict(manifest)
    unhashed.pop("manifest_sha256", None)
    require(
        hashlib.sha256(canonical_bytes(unhashed)).hexdigest() == claimed_sha,
        f"{platform} manifest SHA mismatch",
    )

    authority = manifest.get("authority")
    require(isinstance(authority, dict), f"{platform} authority block missing")
    require(
        authority.get("authoritative_backend") == "rust",
        f"{platform} Rust is not authoritative",
    )
    require(
        authority.get("reverse_shadow_backend") == "cpp",
        f"{platform} C++ reverse shadow missing",
    )
    require(
        authority.get("authoritative_switch_performed") is True,
        f"{platform} authority switch not recorded",
    )
    require(
        authority.get("fallback_permitted") is False,
        f"{platform} silent fallback is permitted",
    )

    parity = manifest.get("parity")
    require(isinstance(parity, dict), f"{platform} parity block missing")
    for key in ("exact_store_tree", "exact_export", "zero_mismatches"):
        require(parity.get(key) is True, f"{platform} {key} is false")
    require(len(str(parity.get("payload_sha256", ""))) == 64, f"{platform} payload SHA invalid")
    require(
        len(str(parity.get("store_tree_sha256", ""))) == 64,
        f"{platform} store-tree SHA invalid",
    )
    require(int(parity.get("store_file_count", 0)) > 0, f"{platform} store inventory empty")

    operations = manifest.get("operations")
    require(isinstance(operations, list), f"{platform} operations missing")
    operation_map = {
        str(item.get("operation")): str(item.get("semantic_sha256"))
        for item in operations
        if isinstance(item, dict) and item.get("passed") is True
    }
    require(set(operation_map) == set(OPERATIONS), f"{platform} operation set mismatch")
    require(
        all(len(operation_map[name]) == 64 for name in OPERATIONS),
        f"{platform} semantic SHA invalid",
    )

    return {
        "manifest_sha256": claimed_sha,
        "parameters": manifest.get("parameters"),
        "payload_sha256": parity["payload_sha256"],
        "store_tree_sha256": parity["store_tree_sha256"],
        "store_file_count": parity["store_file_count"],
        "operation_semantic_sha256": operation_map,
        "total_p50_wall_seconds": manifest.get("total_p50_wall_seconds"),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--linux", type=Path, required=True)
    parser.add_argument("--windows", type=Path, required=True)
    parser.add_argument("--macos", type=Path, required=True)
    parser.add_argument("--commit-sha", required=True)
    parser.add_argument("--z2r3d-head", required=True)
    parser.add_argument("--z2r3d-manifest-sha", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    try:
        require(args.z2r3d_head == EXPECTED_Z2R3D_HEAD, "Z2R-3D head is not canonical")
        require(
            args.z2r3d_manifest_sha == EXPECTED_Z2R3D_MANIFEST,
            "Z2R-3D manifest SHA is not canonical",
        )
        paths = {
            "linux": args.linux,
            "windows": args.windows,
            "macos": args.macos,
        }
        platform_results = {
            platform: validate_platform(
                load_json(paths[platform]), platform, args.commit_sha
            )
            for platform in PLATFORMS
        }

        reference = platform_results["linux"]
        for platform in ("windows", "macos"):
            candidate = platform_results[platform]
            for key in (
                "parameters",
                "payload_sha256",
                "store_tree_sha256",
                "store_file_count",
                "operation_semantic_sha256",
            ):
                require(
                    candidate[key] == reference[key],
                    f"{platform} cross-platform {key} mismatch",
                )

        manifest: dict[str, Any] = {
            "schema": FINAL_SCHEMA,
            "program": "Z2R-3E",
            "commit_sha": args.commit_sha,
            "prerequisite": {
                "z2r3d_head": args.z2r3d_head,
                "z2r3d_manifest_sha256": args.z2r3d_manifest_sha,
            },
            "authority": {
                "authoritative_backend": "rust",
                "reverse_shadow_backend": "cpp",
                "authoritative_switch_performed": True,
                "fallback_permitted": False,
                "rollback_retained": True,
                "rollback": [
                    "ZEVRYON_RUST_MASSIVEDOC_CODEC_AUTHORITATIVE=OFF",
                    "ZEVRYON_RUST_MASSIVEDOC_CODEC_SHADOW=OFF",
                    "ZEVRYON_ENABLE_RUST_CORE=OFF",
                ],
            },
            "parameters": reference["parameters"],
            "parity": {
                "payload_sha256": reference["payload_sha256"],
                "store_tree_sha256": reference["store_tree_sha256"],
                "store_file_count": reference["store_file_count"],
                "operation_semantic_sha256": reference[
                    "operation_semantic_sha256"
                ],
                "exact_cross_platform_parity": True,
                "zero_reverse_shadow_mismatches": True,
            },
            "platforms": platform_results,
            "all_platforms_ready": True,
            "authority_switch_certified": True,
            "promotion_ready": True,
        }
        manifest["manifest_sha256"] = hashlib.sha256(
            canonical_bytes(manifest)
        ).hexdigest()
    except FinalizationError as error:
        print(f"Z2R-3E authority finalization failed: {error}", file=sys.stderr)
        return 1

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(
        json.dumps(
            {
                "schema": manifest["schema"],
                "promotion_ready": manifest["promotion_ready"],
                "manifest_sha256": manifest["manifest_sha256"],
            },
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
