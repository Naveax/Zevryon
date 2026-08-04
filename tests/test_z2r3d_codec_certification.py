from __future__ import annotations

import copy
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
TREE_SHA = "b" * 64


def summary(platform: str, mode: str, multiplier: float = 1.0) -> dict:
    base = 1.0 if mode == "baseline" else 1.1 * multiplier
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
    values = cert.expected_telemetry(operation, RECORDS, CHUNKS)
    return {
        "enabled": True,
        **values,
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
    for name, mismatch in cert.FAULTS.items():
        counters = {
            "record_encode_checks": 0,
            "record_decode_checks": 0,
            "chunk_encode_checks": 0,
            "chunk_decode_checks": 0,
        }
        counters[
            {
                "record-encode": "record_encode_checks",
                "record-decode": "record_decode_checks",
                "chunk-encode": "chunk_encode_checks",
                "chunk-decode": "chunk_decode_checks",
            }[name]
        ] = 1
        faults[name] = {
            "expected_first_mismatch": mismatch,
            "telemetry": {
                "enabled": True,
                **counters,
                "mismatches": 1,
                "first_mismatch": mismatch,
            },
        }
    return {
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
                "store_tree_sha256": TREE_SHA,
                "store_file_count": 14,
            }
            for index in range(3)
        ],
        "export_pairs": [
            {"sample": index, "bytes": LOGICAL_BYTES, "sha256": PAYLOAD_SHA}
            for index in range(3)
        ],
        "faults": faults,
        "canonical_store": {
            "file_count": 14,
            "files": [],
            "tree_sha256": TREE_SHA,
        },
        "payload_sha256": PAYLOAD_SHA,
        "exact_store_tree_parity": True,
        "exact_export_parity": True,
        "zero_shadow_mismatches": True,
        "all_fault_classes_detected": True,
        "report_sha256": "c" * 64,
    }


def platform_manifest(platform: str, commit: str = "d" * 40) -> dict:
    return cert.build_manifest(
        paired_report(platform),
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


def test_platform_manifest_success() -> None:
    manifest = platform_manifest("linux")
    assert manifest["slice_ready"] is True
    assert manifest["promotion_ready"] is False
    assert manifest["operations"][0]["memory"]["metric"] == "peak_pss_bytes"
    assert manifest["authority"]["authoritative_switch_performed"] is False


def test_windows_uses_rss() -> None:
    manifest = platform_manifest("windows")
    assert manifest["operations"][0]["memory"]["metric"] == "peak_rss_bytes"


def test_rejects_forged_verify_chunk_count() -> None:
    report = paired_report("linux")
    for item in report["operations"]["verify"]["shadow_telemetry"]:
        item["chunk_decode_checks"] = CHUNKS - 1
    try:
        cert.validate_report(report, "linux")
    except cert.CertificationError as error:
        assert "chunk_decode_checks" in str(error)
    else:
        raise AssertionError("forged verify chunk count was accepted")


def test_rejects_semantic_or_tree_divergence() -> None:
    report = paired_report("macos")
    report["exact_store_tree_parity"] = False
    try:
        cert.validate_report(report, "macos")
    except cert.CertificationError as error:
        assert "store tree parity" in str(error)
    else:
        raise AssertionError("store-tree divergence was accepted")


def test_rejects_performance_gate() -> None:
    report = paired_report("windows")
    for key in ("p50", "p95", "p99", "maximum"):
        report["operations"]["verify"]["shadow"]["wall_seconds"][key] = 100.0
    try:
        platform_manifest_from_report(report, "windows")
    except cert.CertificationError as error:
        assert "verify.p50" in str(error)
    else:
        raise AssertionError("performance regression was accepted")


def platform_manifest_from_report(report: dict, platform: str) -> dict:
    return cert.build_manifest(
        report,
        platform_name=platform,
        commit_sha="d" * 40,
        compiler="test",
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


def test_rejects_memory_ratio_and_delta() -> None:
    report = paired_report("linux")
    report["operations"]["import"]["shadow"]["peak_pss_bytes"] = {
        "p50": 100 * 1024 * 1024,
        "maximum": 110 * 1024 * 1024,
    }
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


def test_final_manifest_rejects_cross_platform_tree_difference() -> None:
    commit = "1" * 40
    manifests = {
        "linux": platform_manifest("linux", commit),
        "windows": platform_manifest("windows", commit),
        "macos": platform_manifest("macos", commit),
    }
    manifests["macos"] = copy.deepcopy(manifests["macos"])
    manifests["macos"]["parity"]["store_tree_sha256"] = "0" * 64
    try:
        finalize.build_final_manifest(
            manifests,
            commit_sha=commit,
            z2r3c_head="2" * 40,
        )
    except finalize.FinalizationError as error:
        assert "store tree differs" in str(error)
    else:
        raise AssertionError("cross-platform store divergence was accepted")


def test_final_manifest_requires_same_commit() -> None:
    commit = "3" * 40
    manifests = {
        "linux": platform_manifest("linux", commit),
        "windows": platform_manifest("windows", "4" * 40),
        "macos": platform_manifest("macos", commit),
    }
    try:
        finalize.build_final_manifest(
            manifests,
            commit_sha=commit,
            z2r3c_head="5" * 40,
        )
    except finalize.FinalizationError as error:
        assert "windows commit SHA mismatch" in str(error)
    else:
        raise AssertionError("mixed commits were accepted")
