#!/usr/bin/env python3
from __future__ import annotations

import copy
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from typing import Any, Callable

from m8_bundle_common import (
    ARTIFACT_PATHS,
    FINAL_OUTPUT_PATH,
    PLAN_AUTHORITY,
    PLAN_FILENAME,
    PLAN_SCHEMA,
    RECEIPT_PATHS,
    RECEIPT_SCHEMA,
    authority_source_hashes,
    canonical_json,
    clean_git_identity,
    sha256_bytes,
)
from zevryon_platform.performance_contract import DEVICE_PROFILES, TITAN_WORST_CASE, DeviceClass

SOURCE_ROOT = Path(__file__).resolve().parents[1]
BINDER = SOURCE_ROOT / "scripts" / "m8_final_evidence_binder.py"
BUNDLE_ID = "ab" * 16


class TestFailure(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise TestFailure(message)


def process_receipt(invocation: int, start: int, end: int, returncode: int) -> dict[str, int]:
    return {
        "invocation_id": invocation,
        "pid": 10_000 + invocation,
        "started_monotonic_ns": start,
        "ended_monotonic_ns": end,
        "returncode": returncode,
    }


def recovery(generation: int) -> dict[str, Any]:
    return {
        "protocol_present": True,
        "found": True,
        "generation": generation,
        "identity_hex": "".join(f"{(generation * 17 + index) & 0xff:02x}" for index in range(32)),
        "authority_bytes": 160,
        "authority_first": generation & 0xFF,
        "segments": [{"id": 0, "bytes": 7}],
    }


def storage_artifact() -> dict[str, Any]:
    publication_cuts = [
        "after-payload-flush",
        "after-prepare",
        "after-manifest-temp",
        "after-manifest",
        "after-commit",
    ]
    compaction_cuts = ["after-journal-temp", "after-journal-replace", "after-stale-quarantine"]
    invocation = 1
    clock = 1_000

    def next_receipt(returncode: int) -> dict[str, int]:
        nonlocal invocation, clock
        receipt = process_receipt(invocation, clock, clock + 10, returncode)
        invocation += 1
        clock += 20
        return receipt

    publications: list[dict[str, Any]] = []
    for cut in publication_cuts:
        seed = next_receipt(0)
        crash = next_receipt(86)
        after_crash_process = next_receipt(0)
        item: dict[str, Any] = {
            "cut": cut,
            "seed_process": seed,
            "crash_process": crash,
            "recovery_process_after_crash": after_crash_process,
            "injected_exit_code": 86,
            "recovery_after_crash": recovery(2 if cut == "after-commit" else 1),
            "uncommitted_quarantine_before_retry": 0,
            "fresh_process_receipt_verified": True,
            "gate_passed": True,
        }
        if cut != "after-commit":
            item["retry_process"] = next_receipt(0)
            item["recovery_process_after_retry"] = next_receipt(0)
            item["recovery_after_retry"] = recovery(2)
            item["uncommitted_quarantine_after_retry"] = 1 if cut == "after-manifest" else 0
        publications.append(item)

    compactions: list[dict[str, Any]] = []
    for cut in compaction_cuts:
        compactions.append(
            {
                "cut": cut,
                "seed_process": next_receipt(0),
                "crash_process": next_receipt(86),
                "recovery_process_after_crash": next_receipt(0),
                "injected_exit_code": 86,
                "recovery_after_crash": recovery(4),
                "stale_quarantine_before_resume": 1 if cut == "after-stale-quarantine" else 0,
                "resume_process": next_receipt(0),
                "recovery_process_after_resume": next_receipt(0),
                "recovery_after_resume": recovery(4),
                "stale_quarantine_after_resume": 2,
                "fresh_process_receipt_verified": True,
                "gate_passed": True,
            }
        )
    return {
        "schema": "zevryon.m8.storage-process-crash.v1",
        "authority": "m8-fresh-process-storage-crash-cut-recovery-v1",
        "probe_sha256": "11" * 32,
        "injected_crash_exit_code": 86,
        "publication_cuts": publication_cuts,
        "compaction_cuts": compaction_cuts,
        "publication_results": publications,
        "compaction_results": compactions,
        "fresh_process_recovery": True,
        "fresh_process_receipts_verified": True,
        "process_receipt_semantics": "separate-popen-invocation-with-nonoverlapping-monotonic-lifetime-v1",
        "power_loss_certified": False,
        "gate_passed": True,
    }


def profile_artifact(commit: str, tree: str) -> dict[str, Any]:
    observations: list[dict[str, Any]] = []
    envelope = TITAN_WORST_CASE
    for device in DeviceClass:
        profile = DEVICE_PROFILES[device]
        observations.append(
            {
                "device_class": device.value,
                "logical_utf8_bytes": envelope.logical_utf8_bytes,
                "logical_records": envelope.logical_records,
                "logical_nodes": envelope.logical_nodes,
                "style_runs": envelope.style_runs,
                "resource_references": envelope.resource_references,
                "largest_record_bytes": envelope.largest_record_bytes,
                "largest_unbroken_token_bytes": envelope.largest_unbroken_token_bytes,
                "pathological_grapheme_bytes": envelope.pathological_grapheme_bytes,
                "process_group_pss_mb": profile.process_group_pss_target_mb,
                "first_viewport_preindexed_ms": profile.first_viewport_preindexed_ms,
                "first_viewport_streaming_ms": profile.first_viewport_streaming_ms,
                "scroll_p99_ms": profile.scroll_p99_ms,
                "maximum_normal_stall_ms": profile.maximum_normal_stall_ms,
                "exact_search_warm_ms": profile.exact_search_warm_ms,
                "exact_search_cold_ms": profile.exact_search_cold_ms,
                "mutation_p95_us": profile.mutation_p95_us,
                "copy_throughput_mib_s": profile.copy_throughput_mib_s,
                "data_loss_events": 0,
                "invalid_utf8_events": 0,
                "crashes_or_ooms": 0,
            }
        )
    return {
        "schema": "zevryon.m8.profile-observations.v1",
        "candidate_commit": commit,
        "candidate_tree": tree,
        "observations": observations,
    }


def mixed_artifact() -> dict[str, Any]:
    return {
        "schema": "zevryon.m8.mixed-mutation.v2",
        "authority": "m8-sequence-mixed-mutation-integrity-v2",
        "mode": "certification",
        "operations_requested": 10_000_000,
        "operations_completed": 10_000_000,
        "certification_minimum_operations": 10_000_000,
        "certification_threshold_met": True,
        "certification_eligible": True,
        "seed": 42,
        "operation_counts": {
            "insert": 2_000_000,
            "erase": 2_000_000,
            "move": 2_000_000,
            "update_height": 2_000_000,
            "update_summary": 2_000_000,
        },
        "operation_count_sum_matches": True,
        "all_mutation_classes_exercised": True,
        "verification_checkpoints": 2_441,
        "live_logical_order_digest": "1234567890abcdef",
        "oracle_logical_order_digest": "1234567890abcdef",
        "final_record_count": 96,
        "final_text_bytes": 123456,
        "final_layout_height_q8": 987654,
        "final_search_summary": 0xFFFF,
        "logical_order_mismatches": 0,
        "integrity_mismatches": 0,
        "failure_reason": None,
        "gate_passed": True,
    }


def soak_artifact(duration_seconds: int = 86_400) -> bytes:
    checkpoint_ms = 60_000
    target_ms = duration_seconds * 1000
    minimum = max(0, target_ms // checkpoint_ms - 1)
    events: list[dict[str, Any]] = [
        {
            "schema": "zevryon.m8.soak-event.v1",
            "authority": "m8-continuous-dual-mode-soak-v1",
            "event": "start",
            "process_id": 4242,
            "mode": "certification",
            "duration_seconds_requested": duration_seconds,
            "certification_minimum_seconds": 86_400,
            "payload_bytes": 4 * 1024 * 1024,
            "payload_sha256": "22" * 32,
            "checkpoint_interval_ms": checkpoint_ms,
            "memory_sample_interval_ms": 1_000,
            "setup_seconds": 1.25,
        }
    ]
    for ordinal in range(1, minimum + 1):
        elapsed = ordinal * checkpoint_ms
        events.append(
            {
                "schema": "zevryon.m8.soak-event.v1",
                "authority": "m8-continuous-dual-mode-soak-v1",
                "event": "checkpoint",
                "process_id": 4242,
                "ordinal": ordinal,
                "elapsed_ms": elapsed,
                "checkpoint_gap_ms": checkpoint_ms,
                "max_checkpoint_gap_ms": checkpoint_ms,
                "virtualized_queries": ordinal * 10,
                "native_queries": ordinal * 10,
                "rolling_digest": "1111111111111111",
                "current_rss_bytes": 10_000_000,
                "peak_rss_bytes": 11_000_000,
                "memory_samples": ordinal * 60,
                "max_virtualized_query_ms": 1.0,
                "max_native_query_ms": 1.0,
            }
        )
    events.append(
        {
            "schema": "zevryon.m8.soak-event.v1",
            "authority": "m8-continuous-dual-mode-soak-v1",
            "event": "complete",
            "process_id": 4242,
            "mode": "certification",
            "elapsed_ms": target_ms,
            "duration_target_met": True,
            "checkpoint_coverage_met": True,
            "checkpoint_gap_within_limit": True,
            "checkpoint_count": minimum,
            "minimum_checkpoint_count": minimum,
            "max_checkpoint_gap_ms": checkpoint_ms if minimum else 0,
            "virtualized_queries": max(1, minimum * 10),
            "native_queries": max(1, minimum * 10),
            "rolling_digest": "1111111111111111",
            "current_rss_bytes": 10_000_000,
            "peak_rss_bytes": 11_000_000,
            "memory_samples": max(1, minimum * 60),
            "memory_snapshot_failures": 0,
            "query_failures": 0,
            "max_virtualized_query_ms": 1.0,
            "max_native_query_ms": 1.0,
            "certification_eligible": duration_seconds >= 86_400,
            "failure_reason": None,
            "gate_passed": True,
        }
    )
    return ("\n".join(json.dumps(event, separators=(",", ":"), sort_keys=True) for event in events) + "\n").encode()


def fuzz_artifact(cases: int = 10_000) -> dict[str, Any]:
    return {
        "schema": "zevryon.m8.property-fuzz.v1",
        "authority": "m8-four-domain-property-fuzz-v1",
        "mode": "certification",
        "seed": 1234,
        "cases_requested_per_domain": cases,
        "certification_minimum_cases_per_domain": 10_000,
        "certification_threshold_met": cases >= 10_000,
        "certification_eligible": cases >= 10_000,
        "domains": [
            {
                "name": name,
                "cases_completed": cases,
                "failures": 0,
                "digest": f"{index + 1:016x}",
                "failure_case": None,
                "failure_seed": None,
                "failure_reason": None,
            }
            for index, name in enumerate(("unicode", "serializer", "index", "sequence"))
        ],
        "gate_passed": True,
    }


def write_json(path: Path, value: Any) -> bytes:
    raw = canonical_json(value).encode()
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(raw)
    return raw


def create_bundle(root: Path, mutator: Callable[[dict[str, Any], dict[str, bytes]], None] | None = None) -> None:
    commit, tree = clean_git_identity()
    plan = {
        "schema": PLAN_SCHEMA,
        "authority": PLAN_AUTHORITY,
        "bundle_id": BUNDLE_ID,
        "candidate_commit": commit,
        "candidate_tree": tree,
        "created_utc": "2026-09-07T00:00:00Z",
        "artifacts": dict(ARTIFACT_PATHS),
        "receipts": dict(RECEIPT_PATHS),
        "final_output": FINAL_OUTPUT_PATH,
        "authority_source_sha256": authority_source_hashes(),
    }
    write_json(root / PLAN_FILENAME, plan)

    artifact_values: dict[str, bytes] = {
        "profile": canonical_json(profile_artifact(commit, tree)).encode(),
        "storage_crash": canonical_json(storage_artifact()).encode(),
        "mixed_mutation": canonical_json(mixed_artifact()).encode(),
        "soak": soak_artifact(),
        "property_fuzz": canonical_json(fuzz_artifact()).encode(),
    }
    context: dict[str, Any] = {"plan": plan}
    if mutator is not None:
        mutator(context, artifact_values)

    for key, raw in artifact_values.items():
        path = root / ARTIFACT_PATHS[key]
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(raw)

    for key, raw in artifact_values.items():
        if key == "profile":
            receipt = {
                "schema": RECEIPT_SCHEMA,
                "authority": "m8-single-use-profile-import-v1",
                "kind": "profile-import-v1",
                "bundle_id": BUNDLE_ID,
                "candidate_commit": commit,
                "candidate_tree": tree,
                "artifact_key": key,
                "artifact_path": ARTIFACT_PATHS[key],
                "receipt_path": RECEIPT_PATHS[key],
                "imported_utc": "2026-09-07T00:01:00Z",
                "source_path": "/synthetic/profile.json",
                "source_sha256": sha256_bytes(raw),
                "artifact_exists": True,
                "artifact_sha256": sha256_bytes(raw),
                "artifact_bytes": len(raw),
                "candidate_unchanged": True,
                "authority_sources_unchanged": True,
                "evidence_valid": True,
                "recomputed_gate_passed": True,
                "recomputed_profile_gate": {},
                "error": None,
            }
        else:
            receipt = {
                "schema": RECEIPT_SCHEMA,
                "authority": "m8-single-use-bundle-authority-runner-v1",
                "kind": "authority-invocation-v1",
                "bundle_id": BUNDLE_ID,
                "candidate_commit": commit,
                "candidate_tree": tree,
                "artifact_key": key,
                "artifact_path": ARTIFACT_PATHS[key],
                "receipt_path": RECEIPT_PATHS[key],
                "started_utc": "2026-09-07T00:01:00Z",
                "ended_utc": "2026-09-07T00:02:00Z",
                "started_monotonic_ns": 100,
                "ended_monotonic_ns": 200,
                "command": ["/synthetic/authority"],
                "command_file_sha256": {},
                "returncode": 0,
                "launch_error": None,
                "artifact_exists": True,
                "artifact_sha256": sha256_bytes(raw),
                "artifact_bytes": len(raw),
                "candidate_unchanged": True,
                "authority_sources_unchanged": True,
            }
        write_json(root / RECEIPT_PATHS[key], receipt)


def run_binder(root: Path, expected: int) -> dict[str, Any]:
    result = subprocess.run(
        [sys.executable, str(BINDER), "--artifact-root", str(root)],
        text=True,
        encoding="utf-8",
        errors="replace",
        capture_output=True,
        check=False,
        timeout=30.0,
    )
    require(result.returncode == expected, f"binder exit mismatch: expected {expected}, got {result.returncode}; stdout={result.stdout!r}; stderr={result.stderr!r}")
    final = root / FINAL_OUTPUT_PATH
    require(final.is_file(), "binder did not seal final decision")
    return json.loads(final.read_text(encoding="utf-8"))


def main() -> int:
    root_parent = Path(tempfile.mkdtemp(prefix="zevryon-m8-binder-tests-"))
    try:
        valid = root_parent / "valid"
        valid.mkdir()
        create_bundle(valid)
        result = run_binder(valid, 0)
        require(result.get("evidence_valid") is True and result.get("gate_passed") is True, "valid synthetic bundle did not pass")

        def mixed_below(_: dict[str, Any], artifacts: dict[str, bytes]) -> None:
            value = json.loads(artifacts["mixed_mutation"])
            value["operations_requested"] = 9_999_999
            value["operations_completed"] = 9_999_999
            value["operation_counts"] = {"insert": 1_999_999, "erase": 2_000_000, "move": 2_000_000, "update_height": 2_000_000, "update_summary": 2_000_000}
            artifacts["mixed_mutation"] = canonical_json(value).encode()

        case = root_parent / "mixed-below"
        case.mkdir(); create_bundle(case, mixed_below)
        require(run_binder(case, 2).get("evidence_valid") is True, "below-threshold mixed evidence was not a valid gate failure")

        def soak_below(_: dict[str, Any], artifacts: dict[str, bytes]) -> None:
            artifacts["soak"] = soak_artifact(86_399)

        case = root_parent / "soak-below"
        case.mkdir(); create_bundle(case, soak_below)
        require(run_binder(case, 2).get("gate_passed") is False, "86,399-second soak was accepted")

        def fuzz_below(_: dict[str, Any], artifacts: dict[str, bytes]) -> None:
            artifacts["property_fuzz"] = canonical_json(fuzz_artifact(9_999)).encode()

        case = root_parent / "fuzz-below"
        case.mkdir(); create_bundle(case, fuzz_below)
        require(run_binder(case, 2).get("gate_passed") is False, "9,999-case fuzz evidence was accepted")

        def wrong_candidate(_: dict[str, Any], artifacts: dict[str, bytes]) -> None:
            value = json.loads(artifacts["profile"])
            value["candidate_tree"] = "0" * 40
            artifacts["profile"] = canonical_json(value).encode()

        case = root_parent / "wrong-candidate"
        case.mkdir(); create_bundle(case, wrong_candidate)
        require(run_binder(case, 1).get("evidence_valid") is False, "profile candidate-tree drift was not evidence-invalid")

        case = root_parent / "sha-tamper"
        case.mkdir(); create_bundle(case)
        target = case / ARTIFACT_PATHS["mixed_mutation"]
        target.write_bytes(target.read_bytes() + b" ")
        require(run_binder(case, 1).get("evidence_valid") is False, "artifact SHA tamper was not evidence-invalid")

        def missing_cut(_: dict[str, Any], artifacts: dict[str, bytes]) -> None:
            value = json.loads(artifacts["storage_crash"])
            value["publication_cuts"] = value["publication_cuts"][:-1]
            artifacts["storage_crash"] = canonical_json(value).encode()

        case = root_parent / "missing-cut"
        case.mkdir(); create_bundle(case, missing_cut)
        require(run_binder(case, 1).get("evidence_valid") is False, "missing storage cut was not evidence-invalid")
    except (TestFailure, OSError, json.JSONDecodeError, subprocess.SubprocessError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1
    finally:
        shutil.rmtree(root_parent, ignore_errors=True)

    print("Zevryon M8 final evidence binder tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
