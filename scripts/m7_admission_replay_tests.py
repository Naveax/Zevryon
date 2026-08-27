#!/usr/bin/env python3
from __future__ import annotations

import copy
import json
from pathlib import Path
import tempfile

from m7_admission_replay import (
    AdmissionReplayInvalid,
    INPUT_ARTIFACT_KEYS,
    file_sha256,
    replay_admission,
    validate_replay_receipt,
)
from m7_collection_admission import admit_collection
from m7_collection_admission_tests import fixtures


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def require_invalid(callable_, message: str) -> None:
    try:
        callable_()
    except AdmissionReplayInvalid:
        return
    raise AssertionError(message)


def write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def build_bundle(root: Path):
    preflight, browser, virtual, native = fixtures()
    raw = {
        "preflight": preflight,
        "browser_report": browser,
        "zevryon_virtualized": virtual,
        "zevryon_native_dom": native,
    }
    admission = admit_collection(preflight, browser, virtual, native)
    receipts: dict[str, dict[str, str]] = {}
    for name in INPUT_ARTIFACT_KEYS:
        relative = Path("evidence") / f"{name}.json"
        path = root / relative
        write_json(path, raw[name])
        receipts[name] = {
            "path": str(relative),
            "sha256": file_sha256(path),
        }
    admission["input_artifacts"] = receipts
    return admission, raw


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="zevryon-m7-replay-") as temp:
        root = Path(temp)
        admission, raw = build_bundle(root)
        receipt = replay_admission(admission, artifact_root=root)
        validate_replay_receipt(receipt)
        require(receipt["replay_gate_passed"] is True, "valid admission did not replay")
        require(
            set(receipt["input_artifact_sha256"]) == set(INPUT_ARTIFACT_KEYS),
            "replay artifact hash set drifted",
        )

        forged_admission = copy.deepcopy(admission)
        forged_admission["leadership_eligible"] = not bool(admission["leadership_eligible"])
        require_invalid(
            lambda: replay_admission(forged_admission, artifact_root=root),
            "forged admission eligibility was accepted",
        )

        extra_field = copy.deepcopy(admission)
        extra_field["manual_override"] = True
        require_invalid(
            lambda: replay_admission(extra_field, artifact_root=root),
            "extra admission field was accepted",
        )

        browser_path = root / "evidence" / "browser_report.json"
        tampered_browser = copy.deepcopy(raw["browser_report"])
        tampered_browser["corpus_sha256"] = "0" * 64
        write_json(browser_path, tampered_browser)
        tampered_receipt = copy.deepcopy(admission)
        tampered_receipt["input_artifacts"]["browser_report"]["sha256"] = file_sha256(browser_path)
        require_invalid(
            lambda: replay_admission(tampered_receipt, artifact_root=root),
            "raw browser evidence tamper reproduced a valid admission",
        )

        missing_raw = copy.deepcopy(admission)
        missing_raw["input_artifacts"].pop("zevryon_native_dom")
        require_invalid(
            lambda: replay_admission(missing_raw, artifact_root=root),
            "partial raw artifact set was accepted",
        )

        bad_receipt = copy.deepcopy(receipt)
        bad_receipt["replay_gate_passed"] = False
        require_invalid(
            lambda: validate_replay_receipt(bad_receipt),
            "failed replay receipt was accepted",
        )

    print("M7 admission replay authority tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
