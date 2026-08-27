#!/usr/bin/env python3
from __future__ import annotations

import copy
import hashlib
from pathlib import Path
import tempfile

from browser_competitor_registry import CANONICAL_KEYS, get_spec
from m7_admission_replay import (
    RECOMPUTED_ADMISSION_FIELDS,
    REPLAY_AUTHORITY,
    REPLAY_SCHEMA,
)
from m7_collection_admission import ADMISSION_AUTHORITY, ADMISSION_SCHEMA
from m7_evidence_bundle_manifest import (
    BUNDLE_SCHEMA,
    EvidenceBundleInvalid,
    INPUT_ARTIFACT_KEYS,
    build_bundle_manifest,
    canonical_sha256,
    git_source_receipt,
    validate_admission_for_publication,
    validate_bundle_manifest,
    verify_admission_input_artifacts,
)
from m7_leadership_evaluator import EVALUATOR_SCHEMA
from m7_physical_host_evidence import PHYSICAL_HOST_AUTHORITY


SHA_A = "a" * 64
SHA_B = "b" * 64
COMMIT_SHA = "c" * 40
TREE_SHA = "d" * 40


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def require_invalid(callable_, message: str) -> None:
    try:
        callable_()
    except EvidenceBundleInvalid:
        return
    raise AssertionError(message)


def physical_receipt(label: str) -> dict[str, object]:
    return {
        "authority": PHYSICAL_HOST_AUTHORITY,
        "label": label,
        "captured_at_utc": "2026-08-28T00:00:00Z",
        "device_class": "desktop",
        "physical_ram_mib": 32768,
        "logical_cpu_count": 16,
        "cpu_model": "Test CPU",
        "thermal": {
            "state": "nominal",
            "source": "test-fixture",
            "readings_c": [45.0],
        },
        "checks": {
            "physical_device_confirmed": True,
            "thermal_observed": True,
            "device_profile_matches_ram": True,
            "timestamp_is_utc": True,
            "physical_metadata_complete": True,
        },
        "physical_host_gate_passed": True,
    }


def zevryon_physical_stage(mode: str) -> dict[str, object]:
    return {
        "before": physical_receipt(f"zevryon-{mode}-before"),
        "after": physical_receipt(f"zevryon-{mode}-after"),
        "physical_host_gate_passed": True,
    }


def admission(*, eligible: bool = False) -> dict[str, object]:
    bindings = {
        competitor: {
            "adapter": get_spec(competitor).adapter,
            "preflight_runtime_identity": f"{competitor}|preflight",
            "stable_runtime_identity": f"{competitor}|stable",
            "measurement_runtime_identities": [
                f"{competitor}|virtualized",
                f"{competitor}|native-dom",
            ],
            "matched": True,
        }
        for competitor in CANONICAL_KEYS
    }
    return {
        "schema": ADMISSION_SCHEMA,
        "admission_authority": ADMISSION_AUTHORITY,
        "system_fingerprint": SHA_A,
        "corpus_sha256": SHA_B,
        "physical_host_evidence": {
            "runtime_preflight": physical_receipt("runtime-preflight"),
            "browser_full_set": physical_receipt("browser-full-set"),
            "zevryon_virtualized": zevryon_physical_stage("virtualized"),
            "zevryon_native_dom": zevryon_physical_stage("native-dom"),
            "physical_host_gate_passed": True,
        },
        "runtime_bindings": bindings,
        "leadership_evaluation": {
            "schema": EVALUATOR_SCHEMA,
            "leadership_metric_gate_evaluated": True,
            "leadership_eligible": eligible,
            "modes": {},
        },
        "leadership_metric_gate_evaluated": True,
        "leadership_eligible": eligible,
        "input_artifacts": {
            name: {
                "path": f"evidence/{name}.json",
                "sha256": "0" * 64,
            }
            for name in INPUT_ARTIFACT_KEYS
        },
    }


def prepare_artifacts(root: Path, value: dict[str, object]) -> dict[str, dict[str, str]]:
    evidence = root / "evidence"
    evidence.mkdir(parents=True, exist_ok=True)
    artifacts = value["input_artifacts"]
    assert isinstance(artifacts, dict)
    for index, name in enumerate(INPUT_ARTIFACT_KEYS):
        path = evidence / f"{name}.json"
        payload = f"artifact-{index}-{name}\n".encode("utf-8")
        path.write_bytes(payload)
        digest = hashlib.sha256(payload).hexdigest()
        artifacts[name]["sha256"] = digest
    return verify_admission_input_artifacts(value, artifact_root=root)


def replay_receipt(verified: dict[str, dict[str, str]]) -> dict[str, object]:
    return {
        "schema": REPLAY_SCHEMA,
        "replay_authority": REPLAY_AUTHORITY,
        "replay_gate_passed": True,
        "recomputed_fields": list(RECOMPUTED_ADMISSION_FIELDS),
        "input_artifact_sha256": {
            name: str(verified[name]["sha256"]) for name in INPUT_ARTIFACT_KEYS
        },
    }


def source_receipt() -> dict[str, object]:
    return {
        "repository_root": "/repo",
        "commit": COMMIT_SHA,
        "tree": TREE_SHA,
        "tracked_worktree_clean": True,
        "untracked_files_ignored_for_cleanliness": True,
    }


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="zevryon-m7-bundle-") as temp:
        root = Path(temp)
        valid_admission = admission()
        verified = prepare_artifacts(root, valid_admission)
        replay = replay_receipt(verified)
        validate_admission_for_publication(valid_admission)

        manifest = build_bundle_manifest(
            valid_admission,
            admission_path=Path("evidence/admission.json"),
            admission_sha256="e" * 64,
            artifact_receipts=verified,
            admission_replay=replay,
            source=source_receipt(),
        )
        require(manifest["schema"] == BUNDLE_SCHEMA, "bundle schema drifted")
        require(
            manifest["result_class"] == "valid_not_leadership",
            "valid non-leadership bundle result class drifted",
        )
        require(
            manifest["physical_host_evidence"]["physical_host_gate_passed"] is True,
            "physical host evidence was not published",
        )
        require(
            manifest["admission_replay"]["replay_gate_passed"] is True,
            "admission replay evidence was not published",
        )
        validate_bundle_manifest(manifest)
        payload = dict(manifest)
        recorded = payload.pop("manifest_payload_sha256")
        require(recorded == canonical_sha256(payload), "manifest payload SHA drifted")

        leadership_admission = admission(eligible=True)
        leadership_verified = prepare_artifacts(root, leadership_admission)
        leadership_manifest = build_bundle_manifest(
            leadership_admission,
            admission_path=Path("evidence/leadership-admission.json"),
            admission_sha256="f" * 64,
            artifact_receipts=leadership_verified,
            admission_replay=replay_receipt(leadership_verified),
            source=source_receipt(),
        )
        require(
            leadership_manifest["result_class"] == "leadership_eligible",
            "leadership bundle result class drifted",
        )

        missing_physical = copy.deepcopy(valid_admission)
        missing_physical.pop("physical_host_evidence")
        require_invalid(
            lambda: validate_admission_for_publication(missing_physical),
            "admission without physical host evidence was accepted",
        )

        failed_physical = copy.deepcopy(valid_admission)
        failed_physical["physical_host_evidence"]["physical_host_gate_passed"] = False
        require_invalid(
            lambda: validate_admission_for_publication(failed_physical),
            "failed physical host gate was accepted",
        )

        missing_zev_after = copy.deepcopy(valid_admission)
        missing_zev_after["physical_host_evidence"]["zevryon_native_dom"].pop("after")
        require_invalid(
            lambda: validate_admission_for_publication(missing_zev_after),
            "missing Zevryon post-case physical receipt was accepted",
        )

        bad_eligibility = copy.deepcopy(valid_admission)
        bad_eligibility["leadership_evaluation"]["leadership_eligible"] = True
        require_invalid(
            lambda: validate_admission_for_publication(bad_eligibility),
            "admission/evaluator eligibility drift was accepted",
        )

        missing_binding = copy.deepcopy(valid_admission)
        missing_binding["runtime_bindings"].pop(CANONICAL_KEYS[-1])
        require_invalid(
            lambda: validate_admission_for_publication(missing_binding),
            "partial runtime binding set was accepted",
        )

        unmatched_binding = copy.deepcopy(valid_admission)
        unmatched_binding["runtime_bindings"][CANONICAL_KEYS[0]]["matched"] = False
        require_invalid(
            lambda: validate_admission_for_publication(unmatched_binding),
            "unmatched runtime binding was accepted",
        )

        bad_replay = copy.deepcopy(replay)
        bad_replay["input_artifact_sha256"]["browser_report"] = "9" * 64
        require_invalid(
            lambda: build_bundle_manifest(
                valid_admission,
                admission_path=Path("evidence/admission.json"),
                admission_sha256="e" * 64,
                artifact_receipts=verified,
                admission_replay=bad_replay,
                source=source_receipt(),
            ),
            "replay/raw artifact SHA drift was accepted",
        )

        escaped = copy.deepcopy(valid_admission)
        escaped["input_artifacts"]["preflight"]["path"] = "../escape.json"
        require_invalid(
            lambda: verify_admission_input_artifacts(escaped, artifact_root=root),
            "publication verifier accepted an artifact_root escape",
        )

        changed_artifact = root / "evidence" / f"{INPUT_ARTIFACT_KEYS[0]}.json"
        changed_artifact.write_text("tampered\n", encoding="utf-8")
        require_invalid(
            lambda: verify_admission_input_artifacts(
                valid_admission,
                artifact_root=root,
            ),
            "tampered input artifact was accepted",
        )

        forged_manifest = copy.deepcopy(manifest)
        forged_manifest["system_fingerprint"] = "9" * 64
        require_invalid(
            lambda: validate_bundle_manifest(forged_manifest),
            "manifest mutation without payload-SHA update was accepted",
        )

    def clean_git_runner(command: tuple[str, ...]) -> tuple[int, str, str]:
        args = command[3:]
        if args == ("rev-parse", "HEAD"):
            return 0, COMMIT_SHA + "\n", ""
        if args == ("rev-parse", "HEAD^{tree}"):
            return 0, TREE_SHA + "\n", ""
        if args == ("diff", "--quiet", "HEAD", "--"):
            return 0, "", ""
        return 2, "", "unexpected git command"

    receipt = git_source_receipt(
        Path("."),
        expected_commit=COMMIT_SHA,
        runner=clean_git_runner,
    )
    require(receipt["commit"] == COMMIT_SHA, "Git commit receipt drifted")
    require(receipt["tree"] == TREE_SHA, "Git tree receipt drifted")
    require(receipt["tracked_worktree_clean"] is True, "clean Git receipt drifted")

    require_invalid(
        lambda: git_source_receipt(
            Path("."),
            expected_commit="e" * 40,
            runner=clean_git_runner,
        ),
        "wrong expected Git commit was accepted",
    )
    require_invalid(
        lambda: git_source_receipt(
            Path("."),
            expected_commit=123,  # type: ignore[arg-type]
            runner=clean_git_runner,
        ),
        "non-text expected Git commit was accepted",
    )

    def dirty_git_runner(command: tuple[str, ...]) -> tuple[int, str, str]:
        args = command[3:]
        if args == ("rev-parse", "HEAD"):
            return 0, COMMIT_SHA + "\n", ""
        if args == ("rev-parse", "HEAD^{tree}"):
            return 0, TREE_SHA + "\n", ""
        if args == ("diff", "--quiet", "HEAD", "--"):
            return 1, "", ""
        return 2, "", "unexpected git command"

    require_invalid(
        lambda: git_source_receipt(
            Path("."),
            expected_commit=COMMIT_SHA,
            runner=dirty_git_runner,
        ),
        "dirty tracked Git worktree was accepted",
    )

    print("M7 v2 evidence bundle manifest, replay and physical-host authority tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
