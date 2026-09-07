#!/usr/bin/env python3
from __future__ import annotations

import json
from pathlib import Path
import shutil
import sys
import tempfile

SOURCE_ROOT = Path(__file__).resolve().parents[1]
SCRIPT_ROOT = SOURCE_ROOT / "scripts"
for import_root in (SOURCE_ROOT, SCRIPT_ROOT):
    if str(import_root) not in sys.path:
        sys.path.insert(0, str(import_root))

import m8_final_evidence_binder as binder  # noqa: E402
from m8_bundle_common import EvidenceInvalid, canonical_json, sha256_bytes  # noqa: E402


class TestFailure(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise TestFailure(message)


def configure(root: Path) -> tuple[dict[str, object], bytes]:
    plan: dict[str, object] = {
        "bundle_id": "ab" * 16,
        "candidate_commit": "12" * 20,
        "candidate_tree": "34" * 20,
        "final_output": "final-certification.json",
    }
    raw = canonical_json(plan).encode("utf-8")
    binder._current_root = root.resolve()
    binder._current_plan = plan
    binder._current_plan_raw = raw
    return plan, raw


def read_json(path: Path) -> dict[str, object]:
    value = json.loads(path.read_text(encoding="utf-8"))
    require(isinstance(value, dict), f"expected object at {path}")
    return value


def main() -> int:
    parent = Path(tempfile.mkdtemp(prefix="zevryon-m8-final-diagnostic-tests-"))
    try:
        valid_fail_root = parent / "valid-fail"
        plan, plan_raw = configure(valid_fail_root)
        valid_fail_path = valid_fail_root / "final-certification.json"
        valid_fail = {
            "schema": binder.implementation.FINAL_SCHEMA,
            "authority": binder.implementation.FINAL_AUTHORITY,
            "evidence_valid": True,
            "gate_passed": False,
            "error": "synthetic valid gate failure",
        }
        binder._strict_exclusive_write(valid_fail_path, canonical_json(valid_fail))
        written = read_json(valid_fail_path)
        require(written["bundle_id"] == plan["bundle_id"], "valid FAIL lost bundle_id")
        require(written["candidate_commit"] == plan["candidate_commit"], "valid FAIL lost candidate_commit")
        require(written["candidate_tree"] == plan["candidate_tree"], "valid FAIL lost candidate_tree")
        require(written["bundle_plan_sha256"] == sha256_bytes(plan_raw), "valid FAIL lost bundle-plan hash")
        require(written["evidence_valid"] is True and written["gate_passed"] is False, "valid FAIL semantics changed")

        invalid_root = parent / "invalid"
        plan, plan_raw = configure(invalid_root)
        invalid_path = invalid_root / "final-certification.json"
        invalid = {
            "schema": binder.implementation.FINAL_SCHEMA,
            "authority": binder.implementation.FINAL_AUTHORITY,
            "evidence_valid": False,
            "gate_passed": False,
            "error": "synthetic invalid evidence",
        }
        binder._strict_exclusive_write(invalid_path, canonical_json(invalid))
        written = read_json(invalid_path)
        require(written["bundle_id"] == plan["bundle_id"], "invalid decision lost bundle_id")
        require(written["candidate_commit"] == plan["candidate_commit"], "invalid decision lost candidate_commit")
        require(written["candidate_tree"] == plan["candidate_tree"], "invalid decision lost candidate_tree")
        require(written["bundle_plan_sha256"] == sha256_bytes(plan_raw), "invalid decision lost bundle-plan hash")
        require(written["evidence_valid"] is False and written["gate_passed"] is False, "invalid semantics changed")

        pass_root = parent / "pass"
        configure(pass_root)
        pass_path = pass_root / "final-certification.json"
        success = {
            "schema": binder.implementation.FINAL_SCHEMA,
            "authority": binder.implementation.FINAL_AUTHORITY,
            "bundle_id": "ab" * 16,
            "candidate_commit": "12" * 20,
            "candidate_tree": "34" * 20,
            "bundle_plan_sha256": "56" * 32,
            "evidence_valid": True,
            "gate_passed": True,
        }
        binder._strict_exclusive_write(pass_path, canonical_json(success))
        require(read_json(pass_path) == success, "PASS decision was unexpectedly rewritten")

        conflict_root = parent / "conflict"
        configure(conflict_root)
        conflict_path = conflict_root / "final-certification.json"
        conflict = {
            "schema": binder.implementation.FINAL_SCHEMA,
            "authority": binder.implementation.FINAL_AUTHORITY,
            "bundle_id": "cd" * 16,
            "evidence_valid": True,
            "gate_passed": False,
            "error": "synthetic conflict",
        }
        try:
            binder._strict_exclusive_write(conflict_path, canonical_json(conflict))
        except EvidenceInvalid as exc:
            require("conflicts with frozen bundle identity" in str(exc), "wrong conflicting-identity error")
        else:
            raise TestFailure("conflicting final identity was accepted")
        require(not conflict_path.exists(), "conflicting final decision was written")
    except (TestFailure, OSError, json.JSONDecodeError, KeyError, TypeError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1
    finally:
        binder._current_root = None
        binder._current_plan = None
        binder._current_plan_raw = None
        shutil.rmtree(parent, ignore_errors=True)

    print("Zevryon M8 final diagnostic identity binding tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
