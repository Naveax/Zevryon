#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys
from typing import Any

PLATFORM_SCHEMA = "zevryon.rust-shadow-certification.v1"
FINAL_SCHEMA = "zevryon.rust-shadow-promotion-readiness.v1"
PLATFORM_SLICE = "Z2R-1D4-platform-overhead"
EXPECTED_PREREQUISITES = {
    "Z2R-1D1-massivedoc-zenith": "bcee7c0553e69800507d26a50d26d06f8abf6e16bb9f9baae136350503d7ad02",
    "Z2R-1D2-unicode-text": "743300578775305596b2962fd6f68eb22c913bc73f3c1d2c103ff06ce54addbb",
    "Z2R-1D3-font-discovery-fallback-shaping": "17ffa5cfa403b28e744d127b875e06fb9f21edec38a5c916cc45c06610e2d91a",
}


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
    if not isinstance(value, dict):
        raise FinalizationError(f"{path} is not a JSON object")
    return value


def canonical_bytes(value: Any) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode("utf-8")


def validate_windows(report: dict[str, Any]) -> dict[str, Any]:
    require(report.get("schema") == PLATFORM_SCHEMA, "windows schema mismatch")
    require(report.get("slice") == PLATFORM_SLICE, "windows slice mismatch")
    require(report.get("platform") == "windows", "windows identity mismatch")
    require(report.get("adapter") == "directwrite", "windows adapter mismatch")
    require(report.get("slice_ready") is True, "windows slice is not ready")
    require(report.get("promotion_ready") is False, "windows report promoted alone")
    require(len(str(report.get("manifest_sha256", ""))) == 64, "windows manifest SHA invalid")
    authority = report.get("authority")
    require(isinstance(authority, dict), "windows authority block missing")
    require(authority.get("authoritative_backend") == "cpp", "windows authority changed")
    require(authority.get("shadow_backend") == "rust", "windows shadow backend changed")
    require(authority.get("rust_authoritative") is False, "windows Rust became authoritative")
    workloads = report.get("workloads")
    require(isinstance(workloads, list) and len(workloads) == 4, "windows workload scope invalid")
    require(all(item.get("passed") is True for item in workloads), "windows workload failed")
    probe = report.get("probe")
    require(isinstance(probe, dict) and probe.get("mismatches") == 0, "windows probe mismatch")
    return {
        "adapter": report.get("adapter"),
        "manifest_sha256": report["manifest_sha256"],
        "test_count": report.get("tests", {}).get("count"),
        "probe": probe,
        "workloads": workloads,
    }


def build_final_manifest(
    windows: dict[str, Any],
    *,
    commit_sha: str,
    prerequisites: dict[str, str],
) -> dict[str, Any]:
    require(prerequisites == EXPECTED_PREREQUISITES, "prerequisite certification set mismatch")
    windows_summary = validate_windows(windows)
    require(windows.get("commit_sha") == commit_sha, "Windows commit SHA mismatch")

    manifest: dict[str, Any] = {
        "schema": FINAL_SCHEMA,
        "program": "Z2R-1D",
        "commit_sha": commit_sha,
        "mandatory_slices": [
            {"slice": name, "manifest_sha256": digest, "ready": True}
            for name, digest in EXPECTED_PREREQUISITES.items()
        ]
        + [
            {
                "slice": PLATFORM_SLICE,
                "platform": "windows",
                "manifest_sha256": windows["manifest_sha256"],
                "ready": True,
            }
        ],
        "platforms": {"windows": windows_summary},
        "authority": {
            "current_authoritative_backend": "cpp",
            "certified_shadow_backend": "rust",
            "authoritative_switch_performed": False,
            "rollback": [
                "ZEVRYON_ENABLE_RUST_CORE=OFF",
                "ZEVRYON_RUST_LEDGER_SHADOW=OFF",
            ],
        },
        "all_mandatory_slices_ready": True,
        "promotion_ready": True,
        "promotion_scope": "ResourceLedger authority candidate",
        "next_action": "separate reviewed authority-promotion change with rollback retained",
    }
    manifest["manifest_sha256"] = hashlib.sha256(canonical_bytes(manifest)).hexdigest()
    return manifest


def parse_prerequisite(values: list[str]) -> dict[str, str]:
    parsed: dict[str, str] = {}
    for value in values:
        name, separator, digest = value.partition("=")
        if not separator or not name or len(digest) != 64:
            raise FinalizationError(f"invalid prerequisite value: {value}")
        parsed[name] = digest
    return parsed


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--windows", type=Path, required=True)
    parser.add_argument("--commit-sha", required=True)
    parser.add_argument("--prerequisite", action="append", default=[])
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    try:
        manifest = build_final_manifest(
            load_json(args.windows),
            commit_sha=args.commit_sha,
            prerequisites=parse_prerequisite(args.prerequisite),
        )
    except FinalizationError as error:
        print(f"Z2R-1D promotion finalization failed: {error}", file=sys.stderr)
        return 1

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(
        json.dumps(
            {
                "schema": manifest["schema"],
                "promotion_ready": manifest["promotion_ready"],
                "authoritative_switch_performed": manifest["authority"]["authoritative_switch_performed"],
                "manifest_sha256": manifest["manifest_sha256"],
            },
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
