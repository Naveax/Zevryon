#!/usr/bin/env python3
from __future__ import annotations

import copy
import json
from pathlib import Path
import tempfile

from m7_admission_replay_v3 import replay_admission
from m7_admission_replay_v3_tests import write_bundle
from m7_evidence_bundle_manifest_v3 import (
    BUNDLE_SCHEMA,
    EvidenceBundleInvalid,
    build_bundle_manifest,
    canonical_sha256,
    read_admission_snapshot,
    validate_bundle_manifest,
)


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


def source_receipt(*, clean: bool = True) -> dict[str, object]:
    return {
        "repository_root": "/repo",
        "commit": COMMIT_SHA,
        "tree": TREE_SHA,
        "tracked_worktree_clean": clean,
        "untracked_files_ignored_for_cleanliness": True,
    }


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="zevryon-m7-manifest-v3-") as temp:
        root = Path(temp)
        admission, evidence = write_bundle(root)
        admission_path = evidence / "collection-admission-v3.json"
        admission_path.write_text(
            json.dumps(admission, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        snapshot = read_admission_snapshot(
            Path("evidence/collection-admission-v3.json"),
            artifact_root=root,
        )
        replay = replay_admission(snapshot.value, artifact_root=root)
        manifest = build_bundle_manifest(
            snapshot,
            admission_replay=replay,
            source=source_receipt(),
        )
        validate_bundle_manifest(manifest)
        require(manifest["schema"] == BUNDLE_SCHEMA, "v3 manifest schema drifted")
        require(
            manifest["result_class"] == "leadership_eligible",
            "valid leadership fixture result class drifted",
        )
        require(
            manifest["admission_contract"]["recomputed_admission_sha256"]
            == replay["recomputed_admission_sha256"],
            "manifest did not bind replay admission core SHA",
        )

        replay_hash_drift = copy.deepcopy(replay)
        replay_hash_drift["recomputed_admission_sha256"] = "0" * 64
        require_invalid(
            lambda: build_bundle_manifest(
                snapshot,
                admission_replay=replay_hash_drift,
                source=source_receipt(),
            ),
            "replay/admission core SHA drift was accepted",
        )

        replay_count_drift = copy.deepcopy(replay)
        replay_count_drift["input_artifact_byte_count"]["browser_report"] += 1
        require_invalid(
            lambda: build_bundle_manifest(
                snapshot,
                admission_replay=replay_count_drift,
                source=source_receipt(),
            ),
            "replay/raw artifact byte-count drift was accepted",
        )

        require_invalid(
            lambda: build_bundle_manifest(
                snapshot,
                admission_replay=replay,
                source=source_receipt(clean=False),
            ),
            "dirty tracked source receipt was accepted",
        )

        forged_physical = copy.deepcopy(manifest)
        forged_physical["physical_host_evidence"]["browser_full_set"]["after"][
            "cpu_model"
        ] = "forged"
        forged_payload = dict(forged_physical)
        forged_payload.pop("manifest_payload_sha256", None)
        forged_physical["manifest_payload_sha256"] = canonical_sha256(forged_payload)
        require_invalid(
            lambda: validate_bundle_manifest(forged_physical),
            "forged browser post-stage physical receipt was accepted",
        )

        payload_mutation = copy.deepcopy(manifest)
        payload_mutation["result_class"] = "valid_not_leadership"
        require_invalid(
            lambda: validate_bundle_manifest(payload_mutation),
            "manifest mutation without payload-SHA update was accepted",
        )

        contract_mutation = copy.deepcopy(manifest)
        contract_mutation["admission_contract"]["recomputed_admission_sha256"] = "1" * 64
        contract_payload = dict(contract_mutation)
        contract_payload.pop("manifest_payload_sha256", None)
        contract_mutation["manifest_payload_sha256"] = canonical_sha256(contract_payload)
        require_invalid(
            lambda: validate_bundle_manifest(contract_mutation),
            "admission-contract SHA mutation was accepted",
        )

        raw_sha_mutation = copy.deepcopy(manifest)
        raw_sha_mutation["raw_artifacts"]["preflight"]["sha256"] = "2" * 64
        raw_payload = dict(raw_sha_mutation)
        raw_payload.pop("manifest_payload_sha256", None)
        raw_sha_mutation["manifest_payload_sha256"] = canonical_sha256(raw_payload)
        require_invalid(
            lambda: validate_bundle_manifest(raw_sha_mutation),
            "raw/replay artifact SHA drift was accepted",
        )

    with tempfile.TemporaryDirectory(prefix="zevryon-m7-manifest-v3-path-") as temp:
        root = Path(temp)
        outside = root.parent / "zevryon-m7-outside-admission.json"
        outside.write_text("{}\n", encoding="utf-8")
        try:
            require_invalid(
                lambda: read_admission_snapshot(
                    Path("../") / outside.name,
                    artifact_root=root,
                ),
                "admission artifact-root path escape was accepted",
            )
        finally:
            try:
                outside.unlink()
            except OSError:
                pass

    print("M7 evidence bundle manifest v3 authority tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
