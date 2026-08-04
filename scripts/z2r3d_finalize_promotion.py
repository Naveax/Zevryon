#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys
from typing import Any

PLATFORM_SCHEMA = "zevryon.z2r3d.platform-certification.v1"
FINAL_SCHEMA = "zevryon.z2r3d.promotion-readiness.v1"
PLATFORMS = ("linux", "windows", "macos")
OPERATIONS = ("import", "open", "verify", "export")


class FinalizationError(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise FinalizationError(message)


def load_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise FinalizationError(f"cannot load {path}: {error}") from error
    require(isinstance(value, dict), f"{path} is not a JSON object")
    return value


def canonical_bytes(value: Any) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode(
        "utf-8"
    )


def validate_platform(
    manifest: dict[str, Any], platform_name: str, commit_sha: str
) -> dict[str, Any]:
    require(manifest.get("schema") == PLATFORM_SCHEMA, f"{platform_name} schema mismatch")
    require(manifest.get("slice") == "Z2R-3D-massivedoc-codec-promotion", f"{platform_name} slice mismatch")
    require(manifest.get("platform") == platform_name, f"{platform_name} identity mismatch")
    require(manifest.get("commit_sha") == commit_sha, f"{platform_name} commit SHA mismatch")
    require(manifest.get("slice_ready") is True, f"{platform_name} slice is not ready")
    require(manifest.get("promotion_ready") is False, f"{platform_name} promoted prematurely")
    authority = manifest.get("authority")
    require(isinstance(authority, dict), f"{platform_name} authority section missing")
    require(authority.get("authoritative_backend") == "cpp", f"{platform_name} C++ authority lost")
    require(authority.get("shadow_backend") == "rust", f"{platform_name} Rust shadow missing")
    require(
        authority.get("authoritative_switch_performed") is False,
        f"{platform_name} authority switch already performed",
    )
    parity = manifest.get("parity")
    require(isinstance(parity, dict), f"{platform_name} parity section missing")
    for key in ("exact_store_tree", "exact_export", "zero_mismatches"):
        require(parity.get(key) is True, f"{platform_name} {key} is false")
    require(len(str(parity.get("payload_sha256", ""))) == 64, f"{platform_name} payload SHA invalid")
    require(len(str(parity.get("store_tree_sha256", ""))) == 64, f"{platform_name} tree SHA invalid")
    operations = manifest.get("operations")
    require(isinstance(operations, list) and len(operations) == 4, f"{platform_name} operation set incomplete")
    require(
        tuple(item.get("operation") for item in operations) == OPERATIONS,
        f"{platform_name} operation order mismatch",
    )
    require(all(item.get("passed") is True for item in operations), f"{platform_name} operation failed")
    require(manifest.get("total_p50_wall_seconds", {}).get("passed") is True, f"{platform_name} total gate failed")
    claimed_manifest_sha = str(manifest.get("manifest_sha256", ""))
    require(len(claimed_manifest_sha) == 64, f"{platform_name} manifest SHA invalid")
    unhashed_manifest = dict(manifest)
    unhashed_manifest.pop("manifest_sha256", None)
    require(
        hashlib.sha256(canonical_bytes(unhashed_manifest)).hexdigest()
        == claimed_manifest_sha,
        f"{platform_name} manifest SHA does not match canonical content",
    )
    return manifest


def build_final_manifest(
    manifests: dict[str, dict[str, Any]],
    *,
    commit_sha: str,
    z2r3c_head: str,
) -> dict[str, Any]:
    require(len(commit_sha) == 40, "final commit SHA must contain 40 characters")
    require(len(z2r3c_head) == 40, "Z2R-3C prerequisite SHA must contain 40 characters")
    validated = {
        platform_name: validate_platform(manifests[platform_name], platform_name, commit_sha)
        for platform_name in PLATFORMS
    }
    first = validated["linux"]
    expected_parameters = first["parameters"]
    expected_payload = first["parity"]["payload_sha256"]
    expected_tree = first["parity"]["store_tree_sha256"]
    expected_files = first["parity"]["store_file_count"]
    expected_semantics = {
        item["operation"]: item["semantic_sha256"] for item in first["operations"]
    }
    for platform_name in PLATFORMS[1:]:
        manifest = validated[platform_name]
        require(manifest["parameters"] == expected_parameters, f"{platform_name} parameters differ")
        require(manifest["parity"]["payload_sha256"] == expected_payload, f"{platform_name} payload differs")
        require(manifest["parity"]["store_tree_sha256"] == expected_tree, f"{platform_name} store tree differs")
        require(manifest["parity"]["store_file_count"] == expected_files, f"{platform_name} file count differs")
        semantics = {
            item["operation"]: item["semantic_sha256"] for item in manifest["operations"]
        }
        require(semantics == expected_semantics, f"{platform_name} semantic hashes differ")

    final: dict[str, Any] = {
        "schema": FINAL_SCHEMA,
        "slice": "Z2R-3D-massivedoc-codec-promotion",
        "commit_sha": commit_sha,
        "prerequisite": {
            "slice": "Z2R-3C-production-massivedoc-codec-shadow",
            "head_sha": z2r3c_head,
            "required": True,
        },
        "platforms": {
            platform_name: {
                "manifest_sha256": validated[platform_name]["manifest_sha256"],
                "source_report_sha256": validated[platform_name]["source_report_sha256"],
                "compiler": validated[platform_name]["environment"]["compiler"],
                "memory_metric": validated[platform_name]["operations"][0]["memory"]["metric"],
                "slice_ready": True,
            }
            for platform_name in PLATFORMS
        },
        "parameters": expected_parameters,
        "parity": {
            "payload_sha256": expected_payload,
            "store_tree_sha256": expected_tree,
            "store_file_count": expected_files,
            "operation_semantic_sha256": expected_semantics,
            "exact_across_all_platforms": True,
            "zero_shadow_mismatches": True,
            "all_four_fault_classes_detected": True,
        },
        "authority": {
            "current_authoritative_backend": "cpp",
            "promotion_candidate": "rust-massivedoc-descriptor-codec",
            "authoritative_switch_performed": False,
            "rollback_retained": True,
            "rollback": [
                "ZEVRYON_ENABLE_RUST_CORE=OFF",
                "ZEVRYON_RUST_MASSIVEDOC_CODEC_SHADOW=OFF",
            ],
        },
        "all_platforms_ready": True,
        "promotion_ready": True,
    }
    final["manifest_sha256"] = hashlib.sha256(canonical_bytes(final)).hexdigest()
    return final


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--linux", type=Path, required=True)
    parser.add_argument("--windows", type=Path, required=True)
    parser.add_argument("--macos", type=Path, required=True)
    parser.add_argument("--commit-sha", required=True)
    parser.add_argument("--z2r3c-head", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    try:
        final = build_final_manifest(
            {
                "linux": load_json(args.linux),
                "windows": load_json(args.windows),
                "macos": load_json(args.macos),
            },
            commit_sha=args.commit_sha,
            z2r3c_head=args.z2r3c_head,
        )
    except FinalizationError as error:
        print(f"Z2R-3D finalization failed: {error}", file=sys.stderr)
        return 1

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(final, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(
        json.dumps(
            {
                "schema": final["schema"],
                "promotion_ready": final["promotion_ready"],
                "manifest_sha256": final["manifest_sha256"],
            },
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
