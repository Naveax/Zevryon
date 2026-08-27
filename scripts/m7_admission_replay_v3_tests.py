#!/usr/bin/env python3
from __future__ import annotations

import copy
import json
from pathlib import Path
import tempfile

from m7_admission_replay_v3 import (
    AdmissionReplayInvalid,
    INPUT_ARTIFACT_KEYS,
    canonical_sha256,
    recomputed_admission_payload,
    replay_admission,
    validate_replay_receipt,
)
from m7_collection_admission_v3 import admit_collection
from m7_collection_admission_v3_tests import physical_fixtures
from m7_json_artifact_snapshot import read_json_object_snapshot


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def require_invalid(callable_, message: str) -> None:
    try:
        callable_()
    except AdmissionReplayInvalid:
        return
    raise AssertionError(message)


def write_bundle(root: Path):
    preflight, browser, virtual, native = physical_fixtures()
    objects = {
        "preflight": preflight,
        "browser_report": browser,
        "zevryon_virtualized": virtual,
        "zevryon_native_dom": native,
    }
    evidence = root / "evidence"
    evidence.mkdir(parents=True, exist_ok=True)
    receipts = {}
    for name in INPUT_ARTIFACT_KEYS:
        path = evidence / f"{name}.json"
        path.write_text(json.dumps(objects[name], sort_keys=True) + "\n", encoding="utf-8")
        snapshot = read_json_object_snapshot(path, label=name)
        receipts[name] = {
            "path": str(path.relative_to(root)),
            "sha256": snapshot.sha256,
            "byte_count": snapshot.byte_count,
        }
    admission = admit_collection(preflight, browser, virtual, native)
    admission["input_artifacts"] = receipts
    return admission, evidence


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="zevryon-m7-replay-v3-") as temp:
        root = Path(temp)
        admission, evidence = write_bundle(root)
        receipt = replay_admission(admission, artifact_root=root)
        validate_replay_receipt(receipt)
        require(receipt["replay_gate_passed"] is True, "replay gate did not pass")
        require(
            set(receipt["input_artifact_sha256"]) == set(INPUT_ARTIFACT_KEYS),
            "replay SHA receipt set drifted",
        )
        require(
            set(receipt["input_artifact_byte_count"]) == set(INPUT_ARTIFACT_KEYS),
            "replay byte-count receipt set drifted",
        )
        require(
            receipt["recomputed_admission_sha256"]
            == canonical_sha256(recomputed_admission_payload(admission)),
            "recomputed admission SHA did not bind the stored canonical fields",
        )

        forged_receipt = copy.deepcopy(receipt)
        forged_receipt["recomputed_admission_sha256"] = "0" * 64
        validate_replay_receipt(forged_receipt)
        require(
            forged_receipt["recomputed_admission_sha256"]
            != canonical_sha256(recomputed_admission_payload(admission)),
            "forged replay admission hash unexpectedly matched",
        )

        tampered = copy.deepcopy(admission)
        target = evidence / "preflight.json"
        target.write_text(target.read_text(encoding="utf-8") + " ", encoding="utf-8")
        require_invalid(
            lambda: replay_admission(tampered, artifact_root=root),
            "tampered raw artifact was accepted",
        )

    with tempfile.TemporaryDirectory(prefix="zevryon-m7-replay-v3-count-") as temp:
        root = Path(temp)
        admission, _ = write_bundle(root)
        count_drift = copy.deepcopy(admission)
        count_drift["input_artifacts"]["browser_report"]["byte_count"] += 1
        require_invalid(
            lambda: replay_admission(count_drift, artifact_root=root),
            "raw artifact byte-count drift was accepted",
        )

        field_drift = copy.deepcopy(admission)
        field_drift["leadership_eligible"] = not field_drift["leadership_eligible"]
        require_invalid(
            lambda: replay_admission(field_drift, artifact_root=root),
            "stored admission field drift was accepted",
        )

        path_escape = copy.deepcopy(admission)
        path_escape["input_artifacts"]["preflight"]["path"] = "../outside.json"
        require_invalid(
            lambda: replay_admission(path_escape, artifact_root=root),
            "artifact-root path escape was accepted",
        )

        missing_count = copy.deepcopy(admission)
        missing_count["input_artifacts"]["zevryon_native_dom"].pop("byte_count")
        require_invalid(
            lambda: replay_admission(missing_count, artifact_root=root),
            "missing atomic byte-count receipt was accepted",
        )

    print("M7 admission replay v3 tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
