from __future__ import annotations

import copy
import hashlib
import importlib.util
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


cert = load_module(
    "z2r3d_codec_certification",
    ROOT / "scripts" / "z2r3d_codec_certification.py",
)
finalize = load_module(
    "z2r3d_finalize_promotion",
    ROOT / "scripts" / "z2r3d_finalize_promotion.py",
)

RECORDS = 100_000
CHUNKS = 100_004
LOGICAL_BYTES = 128 * 1024 * 1024
PAYLOAD_SHA = "a" * 64


def rehash_report(report: dict) -> dict:
    report.pop("report_sha256", None)
    report["report_sha256"] = hashlib.sha256(cert.canonical_bytes(report)).hexdigest()
    return report


def store_manifest() -> dict:
    files = [
        {
            "path": f"segment-{index:08d}.bin",
            "bytes": index + 1,
            "sha256": f"{index + 10:064x}",
        }
        for index in range(14)
    ]
    manifest = {"file_count": len(files), "files": files}
    manifest["tree_sha256"] = hashlib.sha256(cert.canonical_bytes(manifest)).hexdigest()
    return manifest


def summary(platform: str, mode: str) -> dict:
    base = 1.0 if mode == "baseline" else 1.1
    memory = 20 * 1024 * 1024 if mode == "baseline" else 24 * 1024 * 1024
    return {
        "sample_count": 3,
        "wall_seconds": {
            "p50": base,
            "p95": base * 1.1,
            "p99": base * 1.2,
            "maximum": base * 1.3,
        },
        "internal_seconds": {
            "p50": base * 0.9,
            "p95": base,
            "p99": base * 1.1,
            "maximum": base * 1.2,
        },
        "peak_rss_bytes": {"p50": memory, "maximum": memory + 1024},
        "peak_pss_bytes": (
            {"p50": memory - 1024, "maximum": memory}
            if platform == "linux"
            else None
        ),
    }


def telemetry(operation: str) -> dict:
    return {
        "enabled": True,
        **cert.expected_telemetry(operation, RECORDS, CHUNKS),
        "mismatches": 0,
        "first_mismatch": "None",
    }


def paired_report(platform: str) -> dict:
    operations = {}
    for index, operation in enumerate(cert.OPERATIONS):
        operations[operation] = {
            "semantic_sha256": f"{index + 1:064x}",
            "baseline": summary(platform, "baseline"),
            "shadow": summary(platform, "shadow"),
            "shadow_telemetry": [telemetry(operation) for _ in range(3)],
        }

    faults = {}
    fault_counter = {
        "record-encode": "record_encode_checks",
        "record-decode": "record_decode_checks",
        "chunk-encode": "chunk_encode_checks",
        "chunk-decode": "chunk_decode_checks",
    }
    for name, mismatch in cert.FAULTS.items():
        counters = {counter: 0 for counter in cert.COUNTERS}
        counters[fault_counter[name]] = 1
        faults[name] = {
            "expected_first_mismatch": mismatch,
            "telemetry": {
                "enabled": True,
                **counters,
                "mismatches": 1,
                "first_mismatch": mismatch,
            },
        }

    tree = store_manifest()
    report = {
        "schema": cert.RUN_SCHEMA,
        "platform": platform,
        "parameters": {
            "logical_bytes": LOGICAL_BYTES,
            "records": RECORDS,
            "segment_bytes": 16 * 1024 * 1024,
            "giant_record_bytes": 64 * 1024 * 1024,
            "samples": 3,
        },
        "operations": operations,
        "import_pairs": [
            {
                "sample": index,
                "semantic_sha256": operations["import"]["semantic_sha256"],
                "store_tree_sha256": tree["tree_sha256"],
                "store_file_count": tree["file_count"],
            }
            for index in range(3)
        ],
        "export_pairs": [
            {"sample": index, "bytes": LOGICAL_BYTES, "sha256": PAYLOAD_SHA}
            for index in range(3)
        ],
        "faults": faults,
        "canonical_store": tree,
        "payload_sha256": PAYLOAD_SHA,
        "exact_store_tree_parity": True,
        "exact_export_parity": True,
        "zero_shadow_mismatches": True,
        "all_fault_classes_detected": True,
    }
    return rehash_report(report)


def platform_manifest_from_report(
    report: dict, platform: str, commit: str = "d" * 40
) -> dict:
    return cert.build_manifest(
        report,
        platform_name=platform,
        commit_sha=commit,
        compiler="test-compiler",
        build_type="Release",
        max_p50_ratio=2.0,
        max_p95_ratio=2.25,
        max_p99_ratio=2.5,
        max_maximum_ratio=3.0,
        max_wall_delta_seconds=5.0,
        max_memory_ratio=1.5,
        max_memory_delta_bytes=16 * 1024 * 1024,
        max_total_ratio=2.0,
        max_total_delta_seconds=10.0,
    )


def platform_manifest(platform: str, commit: str = "d" * 40) -> dict:
    return platform_manifest_from_report(paired_report(platform), platform, commit)


def test_platform_manifest_success() -> None:
    manifest = platform_manifest("linux")
    assert manifest["slice_ready"] is True
    assert manifest["promotion_ready"] is False
    assert manifest["operations"][0]["memory"]["metric"] == "peak_pss_bytes"
    assert manifest["authority"]["authoritative_switch_performed"] is False


def test_windows_uses_rss() -> None:
    manifest = platform_manifest("windows")
    assert manifest["operations"][0]["memory"]["metric"] == "peak_rss_bytes"


def test_rejects_invalid_report_hash() -> None:
    report = paired_report("linux")
    report["payload_sha256"] = "0" * 64
    try:
        cert.validate_report(report, "linux")
    except cert.CertificationError as error:
        assert "report SHA-256" in str(error)
    else:
        raise AssertionError("invalid report hash was accepted")


def test_rejects_forged_verify_chunk_count() -> None:
    report = paired_report("linux")
    for item in report["operations"]["verify"]["shadow_telemetry"]:
        item["chunk_decode_checks"] = CHUNKS - 1
    rehash_report(report)
    try:
        cert.validate_report(report, "linux")
    except cert.CertificationError as error:
        assert "chunk_decode_checks" in str(error)
    else:
        raise AssertionError("forged verify chunk count was accepted")


def test_rejects_store_tree_divergence() -> None:
    report = paired_report("macos")
    report["exact_store_tree_parity"] = False
    rehash_report(report)
    try:
        cert.validate_report(report, "macos")
    except cert.CertificationError as error:
        assert "exact_store_tree_parity" in str(error)
    else:
        raise AssertionError("store-tree divergence was accepted")


def test_rejects_forged_store_tree_hash() -> None:
    report = paired_report("windows")
    report["canonical_store"]["files"][0]["bytes"] += 1
    rehash_report(report)
    try:
        cert.validate_report(report, "windows")
    except cert.CertificationError as error:
        assert "store tree SHA" in str(error)
    else:
        raise AssertionError("forged store tree hash was accepted")


def test_rejects_performance_gate() -> None:
    report = paired_report("windows")
    for key in ("p50", "p95", "p99", "maximum"):
        report["operations"]["verify"]["shadow"]["wall_seconds"][key] = 100.0
    rehash_report(report)
    try:
        platform_manifest_from_report(report, "windows")
    except cert.CertificationError as error:
        assert "verify.p50" in str(error)
    else:
        raise AssertionError("performance regression was accepted")


def test_rejects_memory_ratio_and_delta() -> None:
    report = paired_report("linux")
    report["operations"]["import"]["shadow"]["peak_pss_bytes"] = {
        "p50": 100 * 1024 * 1024,
        "maximum": 110 * 1024 * 1024,
    }
    rehash_report(report)
    try:
        platform_manifest_from_report(report, "linux")
    except cert.CertificationError as error:
        assert "peak_pss_bytes" in str(error)
    else:
        raise AssertionError("memory regression was accepted")


def test_final_manifest_success_without_authority_switch() -> None:
    commit = "e" * 40
    final = finalize.build_final_manifest(
        {
            "linux": platform_manifest("linux", commit),
            "windows": platform_manifest("windows", commit),
            "macos": platform_manifest("macos", commit),
        },
        commit_sha=commit,
        z2r3c_head="f" * 40,
    )
    assert final["promotion_ready"] is True
    assert final["all_platforms_ready"] is True
    assert final["authority"]["authoritative_switch_performed"] is False


def test_final_manifest_rejects_forged_platform_manifest_hash() -> None:
    commit = "1" * 40
    manifests = {
        "linux": platform_manifest("linux", commit),
        "windows": platform_manifest("windows", commit),
        "macos": platform_manifest("macos", commit),
    }
    manifests["linux"]["environment"]["compiler"] = "forged"
    try:
        finalize.build_final_manifest(
            manifests,
            commit_sha=commit,
            z2r3c_head="2" * 40,
        )
    except finalize.FinalizationError as error:
        assert "manifest SHA" in str(error)
    else:
        raise AssertionError("forged platform manifest was accepted")


def test_final_manifest_rejects_cross_platform_tree_difference() -> None:
    commit = "3" * 40
    manifests = {
        "linux": platform_manifest("linux", commit),
        "windows": platform_manifest("windows", commit),
        "macos": platform_manifest("macos", commit),
    }
    manifests["macos"] = copy.deepcopy(manifests["macos"])
    manifests["macos"]["parity"]["store_tree_sha256"] = "0" * 64
    unhashed = dict(manifests["macos"])
    unhashed.pop("manifest_sha256", None)
    manifests["macos"]["manifest_sha256"] = hashlib.sha256(
        finalize.canonical_bytes(unhashed)
    ).hexdigest()
    try:
        finalize.build_final_manifest(
            manifests,
            commit_sha=commit,
            z2r3c_head="4" * 40,
        )
    except finalize.FinalizationError as error:
        assert "store tree differs" in str(error)
    else:
        raise AssertionError("cross-platform store divergence was accepted")


def test_final_manifest_requires_same_commit() -> None:
    commit = "5" * 40
    manifests = {
        "linux": platform_manifest("linux", commit),
        "windows": platform_manifest("windows", "6" * 40),
        "macos": platform_manifest("macos", commit),
    }
    try:
        finalize.build_final_manifest(
            manifests,
            commit_sha=commit,
            z2r3c_head="7" * 40,
        )
    except finalize.FinalizationError as error:
        assert "windows commit SHA mismatch" in str(error)
    else:
        raise AssertionError("mixed commits were accepted")
