#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
from typing import Any

REQUIRED_PLATFORMS = ("linux", "windows", "macos")
REQUIRED_FAULTS = ("error", "output", "reset", "state")


def canonical_json(value: Any) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False)


def manifest_sha256(value: dict[str, Any]) -> str:
    payload = dict(value)
    payload.pop("manifest_sha256", None)
    return hashlib.sha256(canonical_json(payload).encode("utf-8")).hexdigest()


def finalize(
    manifests: list[dict[str, Any]],
    *,
    commit_sha: str,
    prerequisite_head: str,
) -> dict[str, Any]:
    if len(manifests) != 3:
        raise ValueError("exactly three platform manifests are required")
    by_platform = {manifest.get("platform"): manifest for manifest in manifests}
    if set(by_platform) != set(REQUIRED_PLATFORMS):
        raise ValueError("Linux, Windows and macOS manifests are required")

    semantic_hashes: set[str] = set()
    logical_bytes: set[int] = set()
    rounds: set[int] = set()
    samples: set[int] = set()
    for platform in REQUIRED_PLATFORMS:
        manifest = by_platform[platform]
        if manifest.get("schema") != "zevryon.z2r3du.platform-certification.v1":
            raise ValueError(f"{platform} has an unexpected schema")
        if manifest.get("commit_sha") != commit_sha:
            raise ValueError(f"{platform} commit does not match the final head")
        if not manifest.get("platform_ready"):
            raise ValueError(f"{platform} is not promotion ready")
        authority = manifest.get("authority", {})
        if authority.get("current") != "cpp":
            raise ValueError(f"{platform} changed the current authority")
        if authority.get("candidate") != "rust":
            raise ValueError(f"{platform} has the wrong candidate backend")
        if authority.get("switch_performed"):
            raise ValueError(f"{platform} performed an authority switch")
        if not authority.get("rollback_retained"):
            raise ValueError(f"{platform} lost rollback")
        if manifest.get("manifest_sha256") != manifest_sha256(manifest):
            raise ValueError(f"{platform} manifest SHA-256 mismatch")
        semantic_hashes.add(str(manifest.get("semantic_sha256")))
        logical_bytes.add(int(manifest.get("logical_bytes", 0)))
        rounds.add(int(manifest.get("rounds", 0)))
        samples.add(int(manifest.get("samples", 0)))

    if len(semantic_hashes) != 1:
        raise ValueError("cross-platform semantic SHA-256 divergence")
    if len(logical_bytes) != 1 or len(rounds) != 1 or len(samples) != 1:
        raise ValueError("cross-platform workload topology divergence")

    linux_faults = tuple(sorted(by_platform["linux"].get("fault_classes", [])))
    if linux_faults != REQUIRED_FAULTS:
        raise ValueError("Linux did not certify all four independent fault classes")
    if by_platform["windows"].get("fault_classes"):
        raise ValueError("Windows unexpectedly contains fault evidence")
    if by_platform["macos"].get("fault_classes"):
        raise ValueError("macOS unexpectedly contains fault evidence")

    output: dict[str, Any] = {
        "schema": "zevryon.z2r3du.promotion-readiness.v1",
        "slice": "Z2R-3D-U",
        "commit_sha": commit_sha,
        "prerequisite": {
            "slice": "Z2R-3C-U",
            "head_sha": prerequisite_head,
        },
        "platforms": {
            platform: {
                "manifest_sha256": by_platform[platform]["manifest_sha256"],
                "ratios": by_platform[platform]["ratios"],
                "peak_rss_delta_bytes": by_platform[platform]["peak_rss_delta_bytes"],
            }
            for platform in REQUIRED_PLATFORMS
        },
        "workload": {
            "logical_bytes": next(iter(logical_bytes)),
            "rounds": next(iter(rounds)),
            "samples": next(iter(samples)),
            "semantic_sha256": next(iter(semantic_hashes)),
            "malformed_classes": 7,
            "discontinuity_cases": 1,
            "output_budget_cases": 1,
            "fault_classes": list(REQUIRED_FAULTS),
        },
        "authority": {
            "current": "cpp",
            "candidate": "rust",
            "switch_performed": False,
            "silent_fallback_permitted": False,
            "rollback_retained": True,
        },
        "all_platforms_ready": True,
        "promotion_ready": True,
    }
    output["manifest_sha256"] = manifest_sha256(output)
    return output


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--linux", required=True)
    parser.add_argument("--windows", required=True)
    parser.add_argument("--macos", required=True)
    parser.add_argument("--commit-sha", required=True)
    parser.add_argument("--prerequisite-head", required=True)
    parser.add_argument("--output", required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    manifests = [
        json.loads(Path(args.linux).read_text(encoding="utf-8")),
        json.loads(Path(args.windows).read_text(encoding="utf-8")),
        json.loads(Path(args.macos).read_text(encoding="utf-8")),
    ]
    result = finalize(
        manifests,
        commit_sha=args.commit_sha,
        prerequisite_head=args.prerequisite_head,
    )
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(json.dumps({
        "promotion_ready": result["promotion_ready"],
        "manifest_sha256": result["manifest_sha256"],
        "semantic_sha256": result["workload"]["semantic_sha256"],
    }, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
