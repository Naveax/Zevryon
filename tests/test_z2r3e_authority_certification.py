from __future__ import annotations

import hashlib
import json
from pathlib import Path
import sys
from typing import Any

import pytest

SCRIPTS = Path(__file__).resolve().parents[1] / "scripts"
if str(SCRIPTS) not in sys.path:
    sys.path.insert(0, str(SCRIPTS))

import z2r3e_finalize_authority as finalizer


COMMIT = "1" * 40


def canonical_bytes(value: Any) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode(
        "utf-8"
    )


def platform_manifest(platform: str, *, store_tree: str = "b" * 64) -> dict[str, Any]:
    manifest: dict[str, Any] = {
        "schema": finalizer.PLATFORM_SCHEMA,
        "slice": "Z2R-3E-massivedoc-codec-authority",
        "platform": platform,
        "commit_sha": COMMIT,
        "slice_ready": True,
        "authority_ready": True,
        "promotion_ready": False,
        "authority": {
            "authoritative_backend": "rust",
            "reverse_shadow_backend": "cpp",
            "authoritative_switch_performed": True,
            "fallback_permitted": False,
            "rollback": [
                "ZEVRYON_RUST_MASSIVEDOC_CODEC_AUTHORITATIVE=OFF",
                "ZEVRYON_RUST_MASSIVEDOC_CODEC_SHADOW=OFF",
                "ZEVRYON_ENABLE_RUST_CORE=OFF",
            ],
        },
        "parameters": {
            "logical_bytes": 134217728,
            "records": 131072,
            "chunks": 131076,
            "segment_bytes": 16777216,
            "giant_record_bytes": 67108864,
            "samples": 3,
        },
        "parity": {
            "payload_sha256": "a" * 64,
            "store_tree_sha256": store_tree,
            "store_file_count": 12,
            "exact_store_tree": True,
            "exact_export": True,
            "zero_mismatches": True,
        },
        "operations": [
            {
                "operation": operation,
                "semantic_sha256": character * 64,
                "passed": True,
            }
            for operation, character in zip(
                finalizer.OPERATIONS, ("c", "d", "e", "f"), strict=True
            )
        ],
        "total_p50_wall_seconds": {
            "baseline": 1.0,
            "shadow": 1.0,
            "ratio": 1.0,
            "delta": 0.0,
            "passed": True,
        },
    }
    manifest["manifest_sha256"] = hashlib.sha256(canonical_bytes(manifest)).hexdigest()
    return manifest


def write_manifest(path: Path, value: dict[str, Any]) -> None:
    path.write_text(json.dumps(value), encoding="utf-8")


def run_finalizer(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
    manifests: dict[str, dict[str, Any]],
) -> tuple[int, Path]:
    paths: dict[str, Path] = {}
    for platform, manifest in manifests.items():
        path = tmp_path / f"{platform}.json"
        write_manifest(path, manifest)
        paths[platform] = path
    output = tmp_path / "final.json"
    monkeypatch.setattr(
        sys,
        "argv",
        [
            "z2r3e_finalize_authority.py",
            "--linux",
            str(paths["linux"]),
            "--windows",
            str(paths["windows"]),
            "--macos",
            str(paths["macos"]),
            "--commit-sha",
            COMMIT,
            "--z2r3d-head",
            finalizer.EXPECTED_Z2R3D_HEAD,
            "--z2r3d-manifest-sha",
            finalizer.EXPECTED_Z2R3D_MANIFEST,
            "--output",
            str(output),
        ],
    )
    return finalizer.main(), output


def test_platform_manifest_accepts_rust_authority() -> None:
    manifest = platform_manifest("linux")
    validated = finalizer.validate_platform(manifest, "linux", COMMIT)
    assert validated["payload_sha256"] == "a" * 64
    assert validated["store_tree_sha256"] == "b" * 64


def test_platform_manifest_rejects_cpp_authority() -> None:
    manifest = platform_manifest("linux")
    manifest["authority"]["authoritative_backend"] = "cpp"
    manifest.pop("manifest_sha256")
    manifest["manifest_sha256"] = hashlib.sha256(canonical_bytes(manifest)).hexdigest()
    with pytest.raises(finalizer.FinalizationError, match="Rust is not authoritative"):
        finalizer.validate_platform(manifest, "linux", COMMIT)


def test_platform_manifest_rejects_forged_hash() -> None:
    manifest = platform_manifest("linux")
    manifest["parity"]["store_file_count"] = 99
    with pytest.raises(finalizer.FinalizationError, match="manifest SHA mismatch"):
        finalizer.validate_platform(manifest, "linux", COMMIT)


def test_finalizer_emits_promotion_ready_manifest(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    code, output = run_finalizer(
        monkeypatch,
        tmp_path,
        {platform: platform_manifest(platform) for platform in finalizer.PLATFORMS},
    )
    assert code == 0
    manifest = json.loads(output.read_text(encoding="utf-8"))
    assert manifest["promotion_ready"] is True
    assert manifest["authority_switch_certified"] is True
    assert manifest["authority"]["authoritative_backend"] == "rust"
    claimed = manifest.pop("manifest_sha256")
    assert claimed == hashlib.sha256(canonical_bytes(manifest)).hexdigest()


def test_finalizer_rejects_cross_platform_store_divergence(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    manifests = {platform: platform_manifest(platform) for platform in finalizer.PLATFORMS}
    manifests["windows"] = platform_manifest("windows", store_tree="9" * 64)
    code, output = run_finalizer(monkeypatch, tmp_path, manifests)
    assert code == 1
    assert not output.exists()


def test_finalizer_rejects_noncanonical_prerequisite(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    paths: dict[str, Path] = {}
    for platform in finalizer.PLATFORMS:
        path = tmp_path / f"{platform}.json"
        write_manifest(path, platform_manifest(platform))
        paths[platform] = path
    output = tmp_path / "final.json"
    monkeypatch.setattr(
        sys,
        "argv",
        [
            "z2r3e_finalize_authority.py",
            "--linux",
            str(paths["linux"]),
            "--windows",
            str(paths["windows"]),
            "--macos",
            str(paths["macos"]),
            "--commit-sha",
            COMMIT,
            "--z2r3d-head",
            "0" * 40,
            "--z2r3d-manifest-sha",
            finalizer.EXPECTED_Z2R3D_MANIFEST,
            "--output",
            str(output),
        ],
    )
    assert finalizer.main() == 1
    assert not output.exists()
