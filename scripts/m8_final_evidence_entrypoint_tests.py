#!/usr/bin/env python3
from __future__ import annotations

import json
from pathlib import Path
import shutil
import sys
import tempfile

import m8_final_evidence_binder_tests as fixtures
from m8_bundle_common import ARTIFACT_PATHS, RECEIPT_PATHS, canonical_json, sha256_bytes


class TestFailure(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise TestFailure(message)


def main() -> int:
    root_parent = Path(tempfile.mkdtemp(prefix="zevryon-m8-binder-entry-tests-"))
    try:
        missing = root_parent / "missing-artifact"
        missing.mkdir()
        fixtures.create_bundle(missing)
        (missing / ARTIFACT_PATHS["property_fuzz"]).unlink()
        result = fixtures.run_binder(missing, 1)
        require(result.get("evidence_valid") is False, "missing raw artifact was not evidence-invalid")
        require("required raw artifact is missing" in str(result.get("error")), "missing-artifact diagnostic drifted")

        malformed = root_parent / "malformed-profile"
        malformed.mkdir()
        fixtures.create_bundle(malformed)
        profile_path = malformed / ARTIFACT_PATHS["profile"]
        profile = json.loads(profile_path.read_text(encoding="utf-8"))
        profile["observations"][0]["collector_gate_passed"] = True
        raw = canonical_json(profile).encode()
        profile_path.write_bytes(raw)

        receipt_path = malformed / RECEIPT_PATHS["profile"]
        receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
        receipt["source_sha256"] = sha256_bytes(raw)
        receipt["artifact_sha256"] = sha256_bytes(raw)
        receipt["artifact_bytes"] = len(raw)
        receipt_path.write_bytes(canonical_json(receipt).encode())

        result = fixtures.run_binder(malformed, 1)
        require(result.get("evidence_valid") is False, "malformed profile was not evidence-invalid")
        require("profile evidence invalid" in str(result.get("error")), "profile-invalid diagnostic was not normalized")
    except (TestFailure, OSError, json.JSONDecodeError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1
    finally:
        shutil.rmtree(root_parent, ignore_errors=True)

    print("Zevryon M8 final binder entrypoint classification tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
