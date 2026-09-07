#!/usr/bin/env python3
from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys
from typing import Any

from m8_bundle_common import (
    ARTIFACT_PATHS,
    FINAL_AUTHORITY,
    FINAL_SCHEMA,
    RECEIPT_SCHEMA,
    EvidenceInvalid,
    GateFailed,
    artifact_path,
    canonical_json,
    exact_keys,
    exclusive_write,
    load_bundle_plan,
    load_json_bytes,
    load_json_path,
    receipt_path,
    require,
    require_bool,
    require_hex64,
    require_int,
    require_str,
    resolve_contained,
    sha256_bytes,
)
from m8_profile_observation_gate import evaluate_document, load_json_strict

PUBLICATION_CUTS = [
    "after-payload-flush",
    "after-prepare",
    "after-manifest-temp",
    "after-manifest",
    "after-commit",
]
COMPACTION_CUTS = [
    "after-journal-temp",
    "after-journal-replace",
    "after-stale-quarantine",
]
FUZZ_DOMAINS = ["unicode", "serializer", "index", "sequence"]
MIXED_OPERATIONS = 10_000_000
SOAK_SECONDS = 86_400
FUZZ_CASES = 10_000
HEX16 = re.compile(r"^[0-9a-f]{16}$")


def gate(condition: bool, message: str) -> None:
    if not condition:
        raise GateFailed(message)


def validate_runner_receipt(
    value: Any,
    key: str,
    plan: dict[str, Any],
    artifact_raw: bytes | None,
) -> dict[str, Any]:
    require(isinstance(value, dict), f"receipt {key} must be an object")
    require(value.get("schema") == RECEIPT_SCHEMA, f"receipt {key} schema mismatch")
    require(value.get("authority") == "m8-single-use-bundle-authority-runner-v1", f"receipt {key} authority mismatch")
    require(value.get("kind") == "authority-invocation-v1", f"receipt {key} kind mismatch")
    require(value.get("bundle_id") == plan["bundle_id"], f"receipt {key} bundle_id mismatch")
    require(value.get("candidate_commit") == plan["candidate_commit"], f"receipt {key} candidate_commit mismatch")
    require(value.get("candidate_tree") == plan["candidate_tree"], f"receipt {key} candidate_tree mismatch")
    require(value.get("artifact_key") == key, f"receipt {key} artifact_key mismatch")
    require(value.get("artifact_path") == plan["artifacts"][key], f"receipt {key} artifact_path mismatch")
    require(value.get("receipt_path") == plan["receipts"][key], f"receipt {key} receipt_path mismatch")
    require_bool(value.get("candidate_unchanged"), f"receipt {key}.candidate_unchanged")
    require_bool(value.get("authority_sources_unchanged"), f"receipt {key}.authority_sources_unchanged")
    require(value["candidate_unchanged"] is True, f"receipt {key} candidate changed during authority run")
    require(value["authority_sources_unchanged"] is True, f"receipt {key} authority sources changed during run")
    started = require_int(value.get("started_monotonic_ns"), f"receipt {key}.started_monotonic_ns")
    ended = require_int(value.get("ended_monotonic_ns"), f"receipt {key}.ended_monotonic_ns")
    require(ended >= started, f"receipt {key} monotonic lifetime is impossible")
    command = value.get("command")
    require(isinstance(command, list) and command and all(isinstance(item, str) for item in command), f"receipt {key} command invalid")
    hashes = value.get("command_file_sha256")
    require(isinstance(hashes, dict), f"receipt {key}.command_file_sha256 must be an object")
    for path, digest in hashes.items():
        require(isinstance(path, str) and path, f"receipt {key} command file path invalid")
        require_hex64(digest, f"receipt {key} command file hash")
    returncode = require_int(value.get("returncode"), f"receipt {key}.returncode", minimum=0)
    artifact_exists = require_bool(value.get("artifact_exists"), f"receipt {key}.artifact_exists")
    if returncode != 0:
        raise GateFailed(f"authority run {key} exited with {returncode}")
    gate(artifact_exists, f"authority run {key} completed without its raw artifact")
    require(artifact_raw is not None, f"artifact {key} is missing despite passing invocation receipt")
    expected_sha = require_hex64(value.get("artifact_sha256"), f"receipt {key}.artifact_sha256")
    require(expected_sha == sha256_bytes(artifact_raw), f"receipt {key} artifact SHA-256 mismatch")
    require(value.get("artifact_bytes") == len(artifact_raw), f"receipt {key} artifact byte count mismatch")
    return value


def validate_profile_receipt(value: Any, plan: dict[str, Any], raw: bytes) -> dict[str, Any]:
    require(isinstance(value, dict), "profile receipt must be an object")
    require(value.get("schema") == RECEIPT_SCHEMA, "profile receipt schema mismatch")
    require(value.get("authority") == "m8-single-use-profile-import-v1", "profile receipt authority mismatch")
    require(value.get("kind") == "profile-import-v1", "profile receipt kind mismatch")
    require(value.get("bundle_id") == plan["bundle_id"], "profile receipt bundle_id mismatch")
    require(value.get("candidate_commit") == plan["candidate_commit"], "profile receipt candidate_commit mismatch")
    require(value.get("candidate_tree") == plan["candidate_tree"], "profile receipt candidate_tree mismatch")
    require(value.get("artifact_key") == "profile", "profile receipt artifact_key mismatch")
    require(value.get("artifact_path") == plan["artifacts"]["profile"], "profile receipt artifact_path mismatch")
    require(value.get("receipt_path") == plan["receipts"]["profile"], "profile receipt receipt_path mismatch")
    require(value.get("candidate_unchanged") is True, "profile candidate changed during import")
    require(value.get("authority_sources_unchanged") is True, "profile authority sources changed during import")
    require(value.get("evidence_valid") is True, "profile importer marked evidence invalid")
    require(value.get("artifact_exists") is True, "profile importer did not preserve raw artifact")
    digest = require_hex64(value.get("artifact_sha256"), "profile receipt artifact_sha256")
    require(digest == sha256_bytes(raw), "profile receipt artifact SHA-256 mismatch")
    require(value.get("artifact_bytes") == len(raw), "profile receipt artifact byte count mismatch")
    return value


def validate_profile(raw: bytes, plan: dict[str, Any]) -> dict[str, Any]:
    document = load_json_strict(raw)
    report, passed = evaluate_document(document, raw)
    require(report["candidate_commit"] == plan["candidate_commit"], "profile raw candidate_commit mismatch")
    require(report["candidate_tree"] == plan["candidate_tree"], "profile raw candidate_tree mismatch")
    gate(passed, "one or more device profiles failed recomputed score_100/Titan gates")
    return report


def expected_identity_hex(generation: int) -> str:
    return "".join(f"{(generation * 17 + index) & 0xff:02x}" for index in range(32))


def validate_recovery(value: Any, generation: int, context: str) -> None:
    require(isinstance(value, dict), f"{context} recovery must be an object")
    gate(value.get("protocol_present") is True, f"{context}: protocol disappeared")
    gate(value.get("found") is True, f"{context}: committed authority missing")
    gate(value.get("generation") == generation, f"{context}: wrong generation")
    gate(value.get("identity_hex") == expected_identity_hex(generation), f"{context}: identity drift")
    gate(value.get("authority_bytes") == 160, f"{context}: authority payload size drift")
    gate(value.get("authority_first") == (generation & 0xFF), f"{context}: authority payload drift")
    gate(value.get("segments") == [{"id": 0, "bytes": 7}], f"{context}: segment inventory drift")


def validate_process_receipt(value: Any, expected_return: int, context: str) -> tuple[int, int, int]:
    fields = {"invocation_id", "pid", "started_monotonic_ns", "ended_monotonic_ns", "returncode"}
    receipt = exact_keys(value, fields, context)
    invocation = require_int(receipt["invocation_id"], f"{context}.invocation_id", 1)
    require_int(receipt["pid"], f"{context}.pid", 1)
    started = require_int(receipt["started_monotonic_ns"], f"{context}.started_monotonic_ns")
    ended = require_int(receipt["ended_monotonic_ns"], f"{context}.ended_monotonic_ns")
    require(ended >= started, f"{context}: impossible process lifetime")
    gate(receipt["returncode"] == expected_return, f"{context}: unexpected process return code")
    return invocation, started, ended


def validate_storage(raw: bytes) -> dict[str, Any]:
    value = load_json_bytes(raw, "storage crash artifact")
    require(isinstance(value, dict), "storage crash artifact must be an object")
    require(value.get("schema") == "zevryon.m8.storage-process-crash.v1", "storage crash schema mismatch")
    require(value.get("authority") == "m8-fresh-process-storage-crash-cut-recovery-v1", "storage crash authority mismatch")
    if value.get("error") is not None and value.get("gate_passed") is False:
        raise GateFailed(f"storage crash authority failed: {value.get('error')}")
    require(value.get("publication_cuts") == PUBLICATION_CUTS, "storage publication cut set/order mismatch")
    require(value.get("compaction_cuts") == COMPACTION_CUTS, "storage compaction cut set/order mismatch")
    require(value.get("injected_crash_exit_code") == 86, "storage injected exit code drifted")
    require(value.get("process_receipt_semantics") == "separate-popen-invocation-with-nonoverlapping-monotonic-lifetime-v1", "storage process receipt semantics drifted")
    require(value.get("power_loss_certified") is False, "process-crash evidence was mislabeled as power-loss certification")
    publications = value.get("publication_results")
    compactions = value.get("compaction_results")
    require(isinstance(publications, list) and len(publications) == len(PUBLICATION_CUTS), "storage publication result count mismatch")
    require(isinstance(compactions, list) and len(compactions) == len(COMPACTION_CUTS), "storage compaction result count mismatch")
    invocation_ids: set[int] = set()

    for index, cut in enumerate(PUBLICATION_CUTS):
        item = publications[index]
        require(isinstance(item, dict) and item.get("cut") == cut, f"publication/{cut}: result identity mismatch")
        seed_id, _, seed_end = validate_process_receipt(item.get("seed_process"), 0, f"publication/{cut}/seed")
        crash_id, crash_start, crash_end = validate_process_receipt(item.get("crash_process"), 86, f"publication/{cut}/crash")
        recovery_id, recovery_start, _ = validate_process_receipt(item.get("recovery_process_after_crash"), 0, f"publication/{cut}/recovery")
        gate(crash_start >= seed_end, f"publication/{cut}: crash started before seed completed")
        gate(recovery_start >= crash_end, f"publication/{cut}: recovery started before crash exited")
        for invocation in (seed_id, crash_id, recovery_id):
            require(invocation not in invocation_ids, f"duplicate process invocation id {invocation}")
            invocation_ids.add(invocation)
        expected_after = 2 if cut == "after-commit" else 1
        validate_recovery(item.get("recovery_after_crash"), expected_after, f"publication/{cut}/after-crash")
        gate(item.get("injected_exit_code") == 86, f"publication/{cut}: crash exit receipt drift")
        if cut != "after-commit":
            retry_id, _, retry_end = validate_process_receipt(item.get("retry_process"), 0, f"publication/{cut}/retry")
            retry_recovery_id, retry_recovery_start, _ = validate_process_receipt(item.get("recovery_process_after_retry"), 0, f"publication/{cut}/retry-recovery")
            gate(retry_recovery_start >= retry_end, f"publication/{cut}: retry recovery started too early")
            for invocation in (retry_id, retry_recovery_id):
                require(invocation not in invocation_ids, f"duplicate process invocation id {invocation}")
                invocation_ids.add(invocation)
            validate_recovery(item.get("recovery_after_retry"), 2, f"publication/{cut}/after-retry")
            expected_quarantine = 1 if cut == "after-manifest" else 0
            gate(item.get("uncommitted_quarantine_after_retry") == expected_quarantine, f"publication/{cut}: uncommitted quarantine count drift")
        else:
            gate(item.get("uncommitted_quarantine_before_retry") == 0, "after-commit was mislabeled as uncommitted quarantine")

    for index, cut in enumerate(COMPACTION_CUTS):
        item = compactions[index]
        require(isinstance(item, dict) and item.get("cut") == cut, f"compaction/{cut}: result identity mismatch")
        seed_id, _, seed_end = validate_process_receipt(item.get("seed_process"), 0, f"compaction/{cut}/seed")
        crash_id, crash_start, crash_end = validate_process_receipt(item.get("crash_process"), 86, f"compaction/{cut}/crash")
        recovery_id, recovery_start, _ = validate_process_receipt(item.get("recovery_process_after_crash"), 0, f"compaction/{cut}/recovery")
        gate(crash_start >= seed_end, f"compaction/{cut}: crash started before seed completed")
        gate(recovery_start >= crash_end, f"compaction/{cut}: recovery started before crash exited")
        resume_id, _, resume_end = validate_process_receipt(item.get("resume_process"), 0, f"compaction/{cut}/resume")
        resume_recovery_id, resume_recovery_start, _ = validate_process_receipt(item.get("recovery_process_after_resume"), 0, f"compaction/{cut}/resume-recovery")
        gate(resume_recovery_start >= resume_end, f"compaction/{cut}: resume recovery started too early")
        for invocation in (seed_id, crash_id, recovery_id, resume_id, resume_recovery_id):
            require(invocation not in invocation_ids, f"duplicate process invocation id {invocation}")
            invocation_ids.add(invocation)
        validate_recovery(item.get("recovery_after_crash"), 4, f"compaction/{cut}/after-crash")
        validate_recovery(item.get("recovery_after_resume"), 4, f"compaction/{cut}/after-resume")
        expected_before = 1 if cut == "after-stale-quarantine" else 0
        gate(item.get("stale_quarantine_before_resume") == expected_before, f"compaction/{cut}: stale quarantine before resume drift")
        gate(item.get("stale_quarantine_after_resume") == 2, f"compaction/{cut}: stale quarantine after resume drift")

    require(value.get("fresh_process_recovery") is True, "storage fresh_process_recovery summary contradicts raw receipts")
    require(value.get("fresh_process_receipts_verified") is True, "storage receipt summary contradicts raw receipts")
    require(value.get("gate_passed") is True, "storage gate_passed summary contradicts recomputed PASS")
    return {"publication_cases": len(publications), "compaction_cases": len(compactions), "process_invocations": len(invocation_ids)}


def validate_mixed(raw: bytes) -> dict[str, Any]:
    value = load_json_bytes(raw, "mixed-mutation artifact")
    require(isinstance(value, dict), "mixed-mutation artifact must be an object")
    require(value.get("schema") == "zevryon.m8.mixed-mutation.v2", "mixed-mutation schema mismatch")
    require(value.get("authority") == "m8-sequence-mixed-mutation-integrity-v2", "mixed-mutation authority mismatch")
    gate(value.get("mode") == "certification", "mixed-mutation artifact is not certification mode")
    requested = require_int(value.get("operations_requested"), "mixed.operations_requested", 1)
    completed = require_int(value.get("operations_completed"), "mixed.operations_completed", 0)
    gate(requested >= MIXED_OPERATIONS, "mixed-mutation requested operation count is below 10,000,000")
    gate(completed >= MIXED_OPERATIONS, "mixed-mutation completed operation count is below 10,000,000")
    gate(completed == requested, "mixed-mutation run did not complete every requested operation")
    gate(value.get("certification_minimum_operations") == MIXED_OPERATIONS, "mixed-mutation certification threshold drifted")
    counts = value.get("operation_counts")
    require(isinstance(counts, dict) and set(counts) == {"insert", "erase", "move", "update_height", "update_summary"}, "mixed-mutation operation count domain mismatch")
    parsed_counts = [require_int(counts[name], f"mixed.operation_counts.{name}") for name in sorted(counts)]
    gate(all(count > 0 for count in parsed_counts), "mixed-mutation did not exercise every mutation class")
    gate(sum(parsed_counts) == completed, "mixed-mutation operation counts do not sum to completed operations")
    gate(require_int(value.get("verification_checkpoints"), "mixed.verification_checkpoints") > 0, "mixed-mutation has no verification checkpoints")
    live = require_str(value.get("live_logical_order_digest"), "mixed live digest")
    oracle = require_str(value.get("oracle_logical_order_digest"), "mixed oracle digest")
    require(HEX16.fullmatch(live) is not None and HEX16.fullmatch(oracle) is not None, "mixed-mutation digest format invalid")
    gate(live != "0000000000000000" and live == oracle, "mixed-mutation live/oracle digest mismatch")
    gate(value.get("logical_order_mismatches") == 0, "mixed-mutation logical-order mismatch recorded")
    gate(value.get("integrity_mismatches") == 0, "mixed-mutation integrity mismatch recorded")
    gate(value.get("failure_reason") is None, "mixed-mutation failure reason recorded")
    recomputed = True
    require(value.get("certification_threshold_met") is recomputed, "mixed certification_threshold_met summary mismatch")
    require(value.get("certification_eligible") is recomputed, "mixed certification_eligible summary mismatch")
    require(value.get("operation_count_sum_matches") is True, "mixed count-sum summary mismatch")
    require(value.get("all_mutation_classes_exercised") is True, "mixed class-coverage summary mismatch")
    require(value.get("gate_passed") is True, "mixed gate_passed summary mismatch")
    return {"operations_completed": completed, "verification_checkpoints": value["verification_checkpoints"], "logical_order_digest": live}


def parse_jsonl(raw: bytes, context: str) -> list[dict[str, Any]]:
    try:
        text = raw.decode("utf-8", errors="strict")
    except UnicodeDecodeError as exc:
        raise EvidenceInvalid(f"{context} is not valid UTF-8") from exc
    lines = [line for line in text.splitlines() if line.strip()]
    require(lines, f"{context} is empty")
    result: list[dict[str, Any]] = []
    for index, line in enumerate(lines, start=1):
        value = load_json_bytes(line.encode("utf-8"), f"{context} line {index}")
        require(isinstance(value, dict), f"{context} line {index} must be an object")
        result.append(value)
    return result


def validate_soak(raw: bytes) -> dict[str, Any]:
    events = parse_jsonl(raw, "continuous-soak artifact")
    gate(len(events) >= 2, "continuous-soak artifact lacks start/complete events")
    start = events[0]
    complete = events[-1]
    checkpoints = events[1:-1]
    for event in events:
        require(event.get("schema") == "zevryon.m8.soak-event.v1", "soak event schema mismatch")
        require(event.get("authority") == "m8-continuous-dual-mode-soak-v1", "soak event authority mismatch")
        gate(event.get("event") != "setup-failure", "continuous soak contains setup-failure evidence")
    require(start.get("event") == "start", "soak first event is not start")
    require(complete.get("event") == "complete", "soak final event is not complete")
    gate(start.get("mode") == "certification", "soak start is not certification mode")
    requested = require_int(start.get("duration_seconds_requested"), "soak duration_seconds_requested", 1)
    gate(requested >= SOAK_SECONDS, "soak requested duration is below 86,400 seconds")
    gate(start.get("certification_minimum_seconds") == SOAK_SECONDS, "soak certification threshold drifted")
    checkpoint_interval = require_int(start.get("checkpoint_interval_ms"), "soak checkpoint_interval_ms", 1)
    gate(checkpoint_interval == 60_000, "soak certification checkpoint interval drifted")
    gate(start.get("memory_sample_interval_ms") == 1_000, "soak certification memory sample interval drifted")
    pid = require_int(start.get("process_id"), "soak start process_id", 1)
    require_hex64(start.get("payload_sha256"), "soak payload_sha256")

    previous_elapsed = 0
    running_max_gap = 0
    previous_vq = 0
    previous_nq = 0
    previous_mem = 0
    for ordinal, event in enumerate(checkpoints, start=1):
        require(event.get("event") == "checkpoint", f"soak middle event {ordinal} is not checkpoint")
        require(event.get("process_id") == pid, "soak process_id changed during run")
        gate(event.get("ordinal") == ordinal, "soak checkpoint ordinal drifted")
        elapsed = require_int(event.get("elapsed_ms"), f"soak checkpoint {ordinal} elapsed_ms", 1)
        gate(elapsed > previous_elapsed, "soak checkpoint elapsed time did not increase")
        gap_ms = require_int(event.get("checkpoint_gap_ms"), f"soak checkpoint {ordinal} gap", 1)
        expected_gap = elapsed if ordinal == 1 else elapsed - previous_elapsed
        gate(gap_ms == expected_gap, "soak checkpoint gap receipt disagrees with elapsed timestamps")
        running_max_gap = max(running_max_gap, gap_ms)
        gate(event.get("max_checkpoint_gap_ms") == running_max_gap, "soak running max checkpoint gap drifted")
        gate(gap_ms <= checkpoint_interval * 2, "soak checkpoint continuity gap exceeded twice the frozen interval")
        vq = require_int(event.get("virtualized_queries"), f"soak checkpoint {ordinal} virtualized_queries")
        nq = require_int(event.get("native_queries"), f"soak checkpoint {ordinal} native_queries")
        mem = require_int(event.get("memory_samples"), f"soak checkpoint {ordinal} memory_samples")
        gate(vq >= previous_vq and nq >= previous_nq and mem >= previous_mem, "soak counters decreased between checkpoints")
        previous_elapsed, previous_vq, previous_nq, previous_mem = elapsed, vq, nq, mem

    require(complete.get("process_id") == pid, "soak process_id changed at completion")
    gate(complete.get("mode") == "certification", "soak completion is not certification mode")
    elapsed = require_int(complete.get("elapsed_ms"), "soak complete elapsed_ms", 1)
    gate(elapsed >= requested * 1000, "soak measured duration did not reach requested certification duration")
    minimum = max(0, (requested * 1000) // checkpoint_interval - 1)
    gate(complete.get("checkpoint_count") == len(checkpoints), "soak checkpoint_count disagrees with raw JSONL")
    gate(complete.get("minimum_checkpoint_count") == minimum, "soak minimum checkpoint count drifted")
    gate(len(checkpoints) >= minimum, "soak checkpoint coverage is insufficient")
    gate(complete.get("max_checkpoint_gap_ms") == running_max_gap, "soak final max checkpoint gap disagrees with raw checkpoints")
    gate(running_max_gap <= checkpoint_interval * 2, "soak final checkpoint gap exceeds limit")
    gate(require_int(complete.get("virtualized_queries"), "soak complete virtualized_queries") > 0, "soak has no virtualized queries")
    gate(require_int(complete.get("native_queries"), "soak complete native_queries") > 0, "soak has no native-dom queries")
    gate(require_int(complete.get("memory_samples"), "soak complete memory_samples") > 0, "soak has no memory samples")
    gate(complete.get("memory_snapshot_failures") == 0, "soak memory snapshot failure recorded")
    gate(complete.get("query_failures") == 0, "soak query failure recorded")
    digest = require_str(complete.get("rolling_digest"), "soak rolling_digest")
    require(HEX16.fullmatch(digest) is not None, "soak rolling digest format invalid")
    gate(digest != "0000000000000000", "soak rolling digest is empty")
    gate(complete.get("failure_reason") is None, "soak failure reason recorded")
    require(complete.get("duration_target_met") is True, "soak duration_target_met summary mismatch")
    require(complete.get("checkpoint_coverage_met") is True, "soak checkpoint_coverage_met summary mismatch")
    require(complete.get("checkpoint_gap_within_limit") is True, "soak checkpoint_gap_within_limit summary mismatch")
    require(complete.get("certification_eligible") is True, "soak certification_eligible summary mismatch")
    require(complete.get("gate_passed") is True, "soak gate_passed summary mismatch")
    return {"elapsed_ms": elapsed, "checkpoint_count": len(checkpoints), "rolling_digest": digest}


def validate_fuzz(raw: bytes) -> dict[str, Any]:
    value = load_json_bytes(raw, "property-fuzz artifact")
    require(isinstance(value, dict), "property-fuzz artifact must be an object")
    require(value.get("schema") == "zevryon.m8.property-fuzz.v1", "property-fuzz schema mismatch")
    require(value.get("authority") == "m8-four-domain-property-fuzz-v1", "property-fuzz authority mismatch")
    gate(value.get("mode") == "certification", "property-fuzz artifact is not certification mode")
    cases = require_int(value.get("cases_requested_per_domain"), "fuzz cases_requested_per_domain", 1)
    gate(cases >= FUZZ_CASES, "property-fuzz case count is below 10,000 per domain")
    gate(value.get("certification_minimum_cases_per_domain") == FUZZ_CASES, "property-fuzz certification threshold drifted")
    domains = value.get("domains")
    require(isinstance(domains, list) and len(domains) == 4, "property-fuzz must contain four domain receipts")
    digests: dict[str, str] = {}
    for index, name in enumerate(FUZZ_DOMAINS):
        item = domains[index]
        require(isinstance(item, dict) and item.get("name") == name, f"property-fuzz domain order/set mismatch at {index}")
        gate(item.get("cases_completed") == cases, f"property-fuzz {name} did not complete every requested case")
        gate(item.get("failures") == 0, f"property-fuzz {name} recorded a failure")
        gate(item.get("failure_case") is None and item.get("failure_seed") is None and item.get("failure_reason") is None, f"property-fuzz {name} contains failure receipts")
        digest = require_str(item.get("digest"), f"property-fuzz {name} digest")
        require(HEX16.fullmatch(digest) is not None, f"property-fuzz {name} digest format invalid")
        gate(digest != "0000000000000000", f"property-fuzz {name} digest is empty")
        digests[name] = digest
    require(value.get("certification_threshold_met") is True, "property-fuzz threshold summary mismatch")
    require(value.get("certification_eligible") is True, "property-fuzz eligibility summary mismatch")
    require(value.get("gate_passed") is True, "property-fuzz gate summary mismatch")
    return {"cases_per_domain": cases, "domain_digests": digests}


def main() -> int:
    parser = argparse.ArgumentParser(description="Recompute the final M8 no-compensation certification from one frozen raw evidence bundle.")
    parser.add_argument("--artifact-root", type=Path, required=True)
    args = parser.parse_args()

    final_path: Path | None = None
    try:
        root = args.artifact_root.resolve()
        plan, plan_raw = load_bundle_plan(root, verify_current=True)
        final_path = resolve_contained(root, plan["final_output"], "final output")
        if final_path.exists():
            raise EvidenceInvalid("final bundle decision is already sealed and may not be overwritten")

        artifacts: dict[str, bytes] = {}
        receipts: dict[str, tuple[Any, bytes]] = {}
        for key in ARTIFACT_PATHS:
            path = artifact_path(root, plan, key)
            gate(path.is_file(), f"required raw artifact is missing: {key}")
            artifacts[key] = path.read_bytes()
            receipt_document, receipt_raw = load_json_path(receipt_path(root, plan, key), f"receipt {key}")
            receipts[key] = (receipt_document, receipt_raw)

        validate_profile_receipt(receipts["profile"][0], plan, artifacts["profile"])
        for key in ("storage_crash", "mixed_mutation", "soak", "property_fuzz"):
            validate_runner_receipt(receipts[key][0], key, plan, artifacts[key])

        gates = {
            "profile_and_titan": validate_profile(artifacts["profile"], plan),
            "storage_crash": validate_storage(artifacts["storage_crash"]),
            "mixed_mutation": validate_mixed(artifacts["mixed_mutation"]),
            "continuous_soak": validate_soak(artifacts["soak"]),
            "property_fuzz": validate_fuzz(artifacts["property_fuzz"]),
        }
        artifact_receipts = {
            key: {
                "path": plan["artifacts"][key],
                "sha256": sha256_bytes(artifacts[key]),
                "bytes": len(artifacts[key]),
                "receipt_path": plan["receipts"][key],
                "receipt_sha256": sha256_bytes(receipts[key][1]),
            }
            for key in ARTIFACT_PATHS
        }
        result = {
            "schema": FINAL_SCHEMA,
            "authority": FINAL_AUTHORITY,
            "bundle_id": plan["bundle_id"],
            "candidate_commit": plan["candidate_commit"],
            "candidate_tree": plan["candidate_tree"],
            "bundle_plan_sha256": sha256_bytes(plan_raw),
            "artifacts": artifact_receipts,
            "recomputed_gates": gates,
            "evidence_valid": True,
            "gate_passed": True,
        }
        exit_code = 0
    except GateFailed as exc:
        if final_path is None:
            print(f"M8 final gate failed before a safe output path could be established: {exc}", file=sys.stderr)
            return 2
        result = {
            "schema": FINAL_SCHEMA,
            "authority": FINAL_AUTHORITY,
            "evidence_valid": True,
            "gate_passed": False,
            "error": str(exc),
        }
        exit_code = 2
    except (EvidenceInvalid, OSError, ValueError, TypeError, KeyError) as exc:
        if final_path is None:
            print(f"M8 final evidence invalid before a safe output path could be established: {exc}", file=sys.stderr)
            return 1
        result = {
            "schema": FINAL_SCHEMA,
            "authority": FINAL_AUTHORITY,
            "evidence_valid": False,
            "gate_passed": False,
            "error": str(exc),
        }
        exit_code = 1

    try:
        exclusive_write(final_path, canonical_json(result))
    except EvidenceInvalid as exc:
        print(f"M8 final decision write failed: {exc}", file=sys.stderr)
        return 1
    print(canonical_json(result), end="")
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
