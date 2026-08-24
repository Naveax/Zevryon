#!/usr/bin/env python3
from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "scripts" / "verify_m5_physical_receipt.py"
SPEC = importlib.util.spec_from_file_location("verify_m5_physical_receipt", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
module = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(module)


def require(value: bool, message: str) -> None:
    if not value:
        raise RuntimeError(message)


def build_bundle(root: Path, expected_sha: str) -> tuple[Path, Path, Path, Path]:
    samples = root / "frame.samples.txt"
    samples.write_text("1.000000000\n2.000000000\n", encoding="utf-8")

    evidence = {
        "schema_version": 1,
        "probe": {"operation": "zenith-tab-runtime-frame-probe"},
        "frame_latency": {
            "schema_version": 1,
            "machine": {
                "physical_device_confirmed": True,
                "thermal": {"state": "nominal", "source": "smoke"},
            },
            "checks": {
                "physical_metadata_complete": True,
                "thermal_state_stable": True,
                "frame_latency_certified": True,
            },
        },
        "checks": {
            "frame_latency_certified": True,
            "native_frame_certified": True,
        },
    }
    evidence_path = root / "frame-certification.json"
    evidence_path.write_text(
        json.dumps(evidence, sort_keys=True, separators=(",", ":")),
        encoding="utf-8",
    )

    manifest = {
        "schema": module.MANIFEST_SCHEMA,
        "artifacts": {
            "evidence_sha256": module._sha256(evidence_path),
            "samples_sha256": module._sha256(samples),
        },
        "certification": evidence,
    }
    manifest_path = root / "manifest.json"
    manifest_path.write_text(
        json.dumps(manifest, sort_keys=True, separators=(",", ":")),
        encoding="utf-8",
    )

    receipt = {
        "schema": module.CANDIDATE_SCHEMA,
        "source": {
            "expected_sha": expected_sha,
            "git_head_sha": expected_sha,
            "exact_head": True,
            "tracked_worktree_clean": True,
        },
        "checks": {
            "exact_candidate_head": True,
            "tracked_worktree_clean": True,
            "native_frame_certified": True,
            "manifest_embeds_exact_evidence": True,
            "candidate_binaries_rebuilt": True,
            "physical_candidate_certified": True,
        },
        "physical": {
            "manifest_sha256": module._sha256(manifest_path),
            "evidence_sha256": module._sha256(evidence_path),
        },
    }
    receipt_path = root / "physical-candidate-receipt.json"
    receipt_path.write_text(
        json.dumps(receipt, sort_keys=True, separators=(",", ":")),
        encoding="utf-8",
    )
    return receipt_path, manifest_path, evidence_path, samples


def test_valid_bundle_and_samples_digest() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        expected_sha = "a" * 40
        receipt, manifest, evidence, samples = build_bundle(root, expected_sha)
        result = module.verify_bundle(
            expected_sha=expected_sha,
            receipt_path=receipt,
            manifest_path=manifest,
            evidence_path=evidence,
            samples_path=samples,
        )
        require(result["physical_receipt_valid"] is True, "valid bundle rejected")
        require(result["expected_sha"] == expected_sha, "verified SHA changed")
        require(
            result["samples_sha256"] == module._sha256(samples),
            "samples digest was not verified",
        )


def test_source_sha_and_tamper_fail_closed() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        expected_sha = "b" * 40
        receipt, manifest, evidence, samples = build_bundle(root, expected_sha)

        try:
            module.verify_bundle(
                expected_sha="c" * 40,
                receipt_path=receipt,
                manifest_path=manifest,
                evidence_path=evidence,
                samples_path=samples,
            )
        except ValueError as exc:
            require("source SHA" in str(exc), "source mismatch diagnostic changed")
        else:
            raise RuntimeError("mismatched source SHA was accepted")

        evidence.write_text("{}\n", encoding="utf-8")
        try:
            module.verify_bundle(
                expected_sha=expected_sha,
                receipt_path=receipt,
                manifest_path=manifest,
                evidence_path=evidence,
                samples_path=samples,
            )
        except ValueError as exc:
            require("digest mismatch" in str(exc), "evidence tamper diagnostic changed")
        else:
            raise RuntimeError("tampered physical evidence was accepted")


def test_unstable_thermal_state_fails_closed() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        expected_sha = "d" * 40
        receipt, manifest, evidence, samples = build_bundle(root, expected_sha)

        evidence_object = json.loads(evidence.read_text(encoding="utf-8"))
        evidence_object["frame_latency"]["machine"]["thermal"]["state"] = "serious"
        evidence.write_text(
            json.dumps(evidence_object, sort_keys=True, separators=(",", ":")),
            encoding="utf-8",
        )
        manifest_object = json.loads(manifest.read_text(encoding="utf-8"))
        manifest_object["certification"] = evidence_object
        manifest_object["artifacts"]["evidence_sha256"] = module._sha256(evidence)
        manifest.write_text(
            json.dumps(manifest_object, sort_keys=True, separators=(",", ":")),
            encoding="utf-8",
        )
        receipt_object = json.loads(receipt.read_text(encoding="utf-8"))
        receipt_object["physical"]["evidence_sha256"] = module._sha256(evidence)
        receipt_object["physical"]["manifest_sha256"] = module._sha256(manifest)
        receipt.write_text(
            json.dumps(receipt_object, sort_keys=True, separators=(",", ":")),
            encoding="utf-8",
        )

        try:
            module.verify_bundle(
                expected_sha=expected_sha,
                receipt_path=receipt,
                manifest_path=manifest,
                evidence_path=evidence,
                samples_path=samples,
            )
        except ValueError as exc:
            require("thermal state" in str(exc), "thermal rejection diagnostic changed")
        else:
            raise RuntimeError("unstable physical thermal state was accepted")


def main() -> int:
    test_valid_bundle_and_samples_digest()
    test_source_sha_and_tamper_fail_closed()
    test_unstable_thermal_state_fails_closed()
    print("Zevryon physical receipt verifier smoke passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
