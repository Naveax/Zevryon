#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import sys
from typing import Any, Mapping

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))
SCRIPTS = ROOT / "scripts"
if str(SCRIPTS) not in sys.path:
    sys.path.insert(0, str(SCRIPTS))

from browser_competitor_benchmark_evidence import normalized_system_fingerprint
from m7_physical_host_evidence import PhysicalHostEvidenceInvalid, certify_physical_host
from zevryon_platform.performance_contract import DEVICE_PROFILES, DeviceClass

PROVENANCE_SCHEMA = "zevryon.m8.profile-case-provenance.v1"
PROVENANCE_AUTHORITY = "m8-profile-case-collector-v1"
CASE_SCHEMA = "zevryon.m8.profile-case.v1"
CASE_AUTHORITY = "m8-raw-profile-case-v1"
HOST_SCHEMA = "zevryon.m8.profile-physical-host.v1"
TITAN_SCHEMA = "zevryon.m8.titan-fixture.v1"
TITAN_AUTHORITY = "m8-canonical-titan-fixture-v1"
PSS_AUTHORITY = "linux-procfs-process-group-smaps-rollup-v1"
VERIFY_SCHEMA = "zevryon.m8.profile-case-provenance-verification.v1"
VERIFY_AUTHORITY = "m8-profile-case-provenance-verifier-v1"
HEX40 = re.compile(r"^[0-9a-f]{40}$")
HEX64 = re.compile(r"^[0-9a-f]{64}$")


class EvidenceInvalid(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise EvidenceInvalid(message)


def load_object(path: Path, label: str) -> tuple[dict[str, Any], bytes]:
    require(path.is_file(), f"{label} file is missing: {path}")
    raw = path.read_bytes()
    try:
        text = raw.decode("utf-8", errors="strict")
        value = json.loads(
            text,
            parse_constant=lambda token: (_ for _ in ()).throw(
                EvidenceInvalid(f"{label} contains non-finite JSON constant: {token}")
            ),
        )
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise EvidenceInvalid(f"{label} is not strict UTF-8 JSON: {exc}") from exc
    require(isinstance(value, dict), f"{label} must be a JSON object")
    return value, raw


def sha256_bytes(raw: bytes) -> str:
    return hashlib.sha256(raw).hexdigest()


def sha256_file(path: Path, chunk_bytes: int = 4 * 1024 * 1024) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        while True:
            chunk = handle.read(chunk_bytes)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def canonical_sha256(value: object) -> str:
    return hashlib.sha256(
        json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
    ).hexdigest()


def hex40(value: object, label: str) -> str:
    require(isinstance(value, str) and HEX40.fullmatch(value) is not None, f"{label} must be lowercase 40-hex")
    return value


def hex64(value: object, label: str) -> str:
    require(isinstance(value, str) and HEX64.fullmatch(value) is not None, f"{label} must be lowercase 64-hex")
    return value


def number(value: object, label: str) -> float:
    require(type(value) in {int, float}, f"{label} must be numeric")
    result = float(value)
    require(math.isfinite(result) and result >= 0.0, f"{label} must be finite and non-negative")
    return result


def integer(value: object, label: str) -> int:
    require(type(value) is int and value >= 0, f"{label} must be a non-negative integer")
    return value


def same_float(left: object, right: object, label: str, tolerance: float = 1e-9) -> None:
    lhs = number(left, f"{label}.left")
    rhs = number(right, f"{label}.right")
    scale = max(1.0, abs(lhs), abs(rhs))
    require(abs(lhs - rhs) <= tolerance * scale, f"{label} drifted: {lhs} != {rhs}")


def validate_physical_host_receipt(host: Mapping[str, Any], expected_device: str) -> dict[str, Any]:
    require(host.get("schema") == HOST_SCHEMA, "physical host schema mismatch")
    require(host.get("physical_host_gate_passed") is True, "physical host gate did not pass")
    require(host.get("device_class") == expected_device, "physical host device class differs from case")
    require(host.get("same_system_fingerprint") is True, "physical host receipt does not assert stable system identity")

    try:
        device = DeviceClass(expected_device)
    except (TypeError, ValueError) as exc:
        raise EvidenceInvalid(f"invalid physical host device class: {expected_device!r}") from exc
    profile = DEVICE_PROFILES[device]

    fingerprints: list[str] = []
    physical_ram_values: list[int] = []
    for stage in ("before", "after"):
        block = host.get(stage)
        require(isinstance(block, dict), f"physical host {stage} block is missing")
        raw_host = block.get("host")
        require(isinstance(raw_host, dict), f"physical host {stage}.host is missing")
        certification = block.get("certification")
        require(isinstance(certification, dict), f"physical host {stage}.certification is missing")
        try:
            recomputed = certify_physical_host(raw_host, label=f"m8-profile-{stage}-verification")
            fingerprint = normalized_system_fingerprint(raw_host)
        except (PhysicalHostEvidenceInvalid, TypeError, ValueError) as exc:
            raise EvidenceInvalid(f"physical host {stage} certification failed: {exc}") from exc
        require(recomputed.get("physical_host_gate_passed") is True, f"physical host {stage} did not recertify")
        require(recomputed.get("device_class") == expected_device, f"physical host {stage} device class drifted")
        require(certification.get("authority") == recomputed.get("authority"), f"physical host {stage} authority drifted")
        require(certification.get("device_class") == expected_device, f"physical host {stage} certification class drifted")
        require(certification.get("physical_host_gate_passed") is True, f"physical host {stage} stored gate did not pass")
        require(certification.get("checks") == recomputed.get("checks"), f"physical host {stage} certification checks drifted")
        require(certification.get("physical_ram_mib") == recomputed.get("physical_ram_mib"), f"physical host {stage} RAM receipt drifted")
        thermal = recomputed.get("thermal")
        require(isinstance(thermal, dict), f"physical host {stage} thermal receipt is missing")
        require(thermal.get("state") not in {"serious", "critical"}, f"physical host {stage} thermal state is not admission-stable")
        ram = raw_host.get("physical_ram_mib")
        require(type(ram) is int and ram >= profile.minimum_physical_ram_mib, f"physical host {stage} RAM is below profile minimum")
        require(raw_host.get("device_class") == expected_device, f"physical host {stage} raw device class drifted")
        fingerprints.append(fingerprint)
        physical_ram_values.append(ram)

    require(fingerprints[0] == fingerprints[1], "physical host fingerprint changed between before/after receipts")
    require(host.get("system_fingerprint") == fingerprints[0], "physical host top-level fingerprint drifted")
    require(physical_ram_values[0] == physical_ram_values[1], "physical RAM changed between before/after receipts")
    return {
        "system_fingerprint": fingerprints[0],
        "physical_ram_mib": physical_ram_values[0],
        "before_and_after_recertified": True,
    }


def validate_pss(provenance: Mapping[str, Any], case: Mapping[str, Any]) -> dict[str, Any]:
    block = provenance.get("process_group_pss")
    require(isinstance(block, dict), "provenance process_group_pss block is missing")
    require(block.get("authority") == PSS_AUTHORITY, "PSS authority mismatch")
    require(block.get("expected_single_process") is True, "profile probe must remain a single-process authority")
    require(block.get("process_group_complete") is True, "process-group PSS receipt is incomplete")
    root_pid = integer(block.get("root_pid"), "process_group_pss.root_pid")
    observed_pids = block.get("observed_pids")
    require(isinstance(observed_pids, list) and observed_pids == [root_pid], "observed PIDs are not exactly the root probe PID")
    samples = block.get("samples")
    require(isinstance(samples, list) and samples, "PSS sample array is empty")

    recomputed_mb: list[float] = []
    previous_ns = -1
    previous_elapsed = -1.0
    for index, sample in enumerate(samples):
        require(isinstance(sample, dict), f"PSS sample {index} must be an object")
        monotonic_ns = integer(sample.get("monotonic_ns"), f"PSS sample {index}.monotonic_ns")
        elapsed_ms = number(sample.get("elapsed_ms"), f"PSS sample {index}.elapsed_ms")
        require(monotonic_ns > previous_ns, "PSS monotonic timestamps are not strictly increasing")
        require(elapsed_ms >= previous_elapsed, "PSS elapsed timestamps moved backwards")
        previous_ns = monotonic_ns
        previous_elapsed = elapsed_ms
        pids = sample.get("pids")
        require(pids == [root_pid], f"PSS sample {index} process set drifted")
        per_pid = sample.get("per_pid_pss_kib")
        require(isinstance(per_pid, dict) and set(per_pid) == {str(root_pid)}, f"PSS sample {index} per-PID table drifted")
        total_kib = integer(per_pid[str(root_pid)], f"PSS sample {index} root PSS")
        expected_mb = total_kib * 1024.0 / 1_000_000.0
        same_float(sample.get("aggregate_pss_mb"), expected_mb, f"PSS sample {index} aggregate")
        recomputed_mb.append(expected_mb)

    raw = case.get("raw")
    require(isinstance(raw, dict), "profile case raw block is missing")
    case_pss = raw.get("pss_samples_mb")
    require(isinstance(case_pss, list) and len(case_pss) == len(recomputed_mb), "case PSS sample count differs from provenance")
    for index, expected in enumerate(recomputed_mb):
        same_float(case_pss[index], expected, f"case PSS sample {index}")
    return {
        "sample_count": len(recomputed_mb),
        "root_pid": root_pid,
        "maximum_aggregate_pss_mb": max(recomputed_mb),
    }


def validate_phase_binding(provenance: Mapping[str, Any], case: Mapping[str, Any]) -> dict[str, Any]:
    phase = provenance.get("phase_receipts")
    require(isinstance(phase, dict), "phase_receipts are missing")
    require(phase.get("schema") == "zevryon.m8.profile-probe.v1", "phase receipt schema mismatch")
    require(phase.get("device_class") == case.get("device_class"), "phase receipt device class mismatch")
    policy = phase.get("runtime_policy")
    require(isinstance(policy, dict), "phase runtime policy is missing")
    require(policy.get("layout_store_read_policy_applied") is True, "layout measurement did not use the profile StoreReadConfig")
    require(policy.get("within_profile_budgets") is True, "phase runtime policy exceeded cache budgets")

    case_policy = case.get("runtime_policy")
    require(isinstance(case_policy, dict), "case runtime policy is missing")
    for key in (
        "profile", "within_profile_budgets", "hot_budget_bytes", "hot_allocated_bytes",
        "warm_budget_bytes", "warm_allocated_bytes", "cold_budget_bytes", "cold_allocated_bytes",
    ):
        require(case_policy.get(key) == policy.get(key), f"case runtime policy field drifted: {key}")

    raw = case.get("raw")
    require(isinstance(raw, dict), "case raw block is missing")
    streaming = phase.get("streaming")
    preindexed = phase.get("preindexed")
    scroll = phase.get("scroll")
    search = phase.get("search")
    mutation = phase.get("mutation")
    copy_phase = phase.get("copy")
    for value, label in ((streaming, "streaming"), (preindexed, "preindexed"), (scroll, "scroll"), (search, "search"), (mutation, "mutation"), (copy_phase, "copy")):
        require(isinstance(value, dict), f"phase {label} receipt is missing")

    same_float(raw.get("first_viewport_streaming_ms"), streaming.get("milliseconds"), "streaming first viewport")
    same_float(raw.get("first_viewport_preindexed_ms"), preindexed.get("milliseconds"), "preindexed first viewport")
    require(raw.get("scroll_samples_ms") == scroll.get("samples_ms"), "scroll sample array differs from phase receipt")
    same_float(raw.get("exact_search_cold_ms"), search.get("cold_ms"), "cold exact search")
    same_float(raw.get("exact_search_warm_ms"), search.get("warm_ms"), "warm exact search")
    require(raw.get("mutation_samples_us") == mutation.get("samples_us"), "mutation sample array differs from phase receipt")

    copy_raw = raw.get("copy")
    require(isinstance(copy_raw, dict), "case copy block is missing")
    for key in ("source_bytes", "output_bytes", "cancelled"):
        require(copy_raw.get(key) == copy_phase.get(key), f"case copy field differs from phase receipt: {key}")
    same_float(copy_raw.get("elapsed_seconds"), copy_phase.get("elapsed_seconds"), "copy elapsed time")
    require(raw.get("probe_return_code") == 0, "case probe return code is not zero")
    require(raw.get("probe_terminated_abnormally") is False, "case reports abnormal probe termination")
    return {
        "layout_store_read_policy_applied": True,
        "scroll_samples": len(scroll.get("samples_ms", [])),
        "mutation_samples": len(mutation.get("samples_us", [])),
    }


def verify(
    *,
    provenance_path: Path,
    case_path: Path,
    host_path: Path,
    titan_report_path: Path,
    titan_path: Path,
    copy_output_path: Path,
    probe_path: Path,
) -> dict[str, Any]:
    provenance, provenance_raw = load_object(provenance_path, "profile provenance")
    case, case_raw = load_object(case_path, "profile case")
    host, host_raw = load_object(host_path, "physical host")
    titan, titan_raw = load_object(titan_report_path, "Titan report")

    require(provenance.get("schema") == PROVENANCE_SCHEMA, "profile provenance schema mismatch")
    require(provenance.get("authority") == PROVENANCE_AUTHORITY, "profile provenance authority mismatch")
    require(provenance.get("collector_gate_passed") is True, "profile collector gate did not pass")
    require(case.get("schema") == CASE_SCHEMA, "profile case schema mismatch")
    require(case.get("authority") == CASE_AUTHORITY, "profile case authority mismatch")
    require(titan.get("schema") == TITAN_SCHEMA, "Titan report schema mismatch")
    require(titan.get("authority") == TITAN_AUTHORITY, "Titan authority mismatch")
    require(titan.get("mode") == "certification", "profile provenance requires certification Titan evidence")
    require(titan.get("certification_eligible") is True and titan.get("gate_passed") is True, "Titan evidence is not certification eligible")

    commit = hex40(provenance.get("candidate_commit"), "provenance candidate_commit")
    tree = hex40(provenance.get("candidate_tree"), "provenance candidate_tree")
    require(case.get("candidate_commit") == commit and titan.get("candidate_commit") == commit, "candidate commit binding drifted")
    require(case.get("candidate_tree") == tree and titan.get("candidate_tree") == tree, "candidate tree binding drifted")
    require(case.get("device_class") == provenance.get("device_class"), "device class binding drifted")
    require(case.get("case_id") == provenance.get("case_id"), "case_id binding drifted")

    case_ref = provenance.get("case")
    require(isinstance(case_ref, dict), "provenance case reference is missing")
    actual_case_sha = sha256_bytes(case_raw)
    require(hex64(case_ref.get("sha256"), "provenance case SHA") == actual_case_sha, "profile case SHA-256 mismatch")

    host_ref = provenance.get("physical_host")
    require(isinstance(host_ref, dict), "provenance physical-host reference is missing")
    actual_host_sha = sha256_bytes(host_raw)
    require(hex64(host_ref.get("sha256"), "provenance host SHA") == actual_host_sha, "physical host SHA-256 mismatch")
    host_summary = validate_physical_host_receipt(host, str(case.get("device_class")))
    case_host = case.get("physical_host")
    require(isinstance(case_host, dict), "case physical_host block is missing")
    require(case_host.get("receipt_sha256") == actual_host_sha, "case physical-host receipt SHA mismatch")
    require(case_host.get("qualified") is True and case_host.get("process_group_complete") is True, "case physical host is not fully qualified")
    require(case_host.get("pss_authority") == "aggregate-pss", "case PSS authority drifted")
    require(case_host.get("physical_memory_mib") == host_summary["physical_ram_mib"], "case physical RAM differs from recertified host")
    require(host_ref.get("system_fingerprint") == host_summary["system_fingerprint"], "physical system fingerprint drifted")

    titan_ref = provenance.get("titan")
    require(isinstance(titan_ref, dict), "provenance Titan reference is missing")
    actual_titan_report_sha = sha256_bytes(titan_raw)
    require(hex64(titan_ref.get("report_sha256"), "provenance Titan report SHA") == actual_titan_report_sha, "Titan report SHA-256 mismatch")
    generation = titan.get("generation")
    require(isinstance(generation, dict), "Titan generation receipt is missing")
    actual_titan_sha = sha256_file(titan_path)
    require(actual_titan_sha == generation.get("container_sha256") == titan_ref.get("container_sha256"), "Titan container SHA-256 binding drifted")
    require(generation.get("payload_sha256") == titan_ref.get("payload_sha256"), "Titan payload SHA-256 binding drifted")
    case_titan = case.get("titan")
    require(isinstance(case_titan, dict), "case Titan reference is missing")
    require(case_titan.get("report_sha256") == actual_titan_report_sha, "case Titan report SHA mismatch")
    require(case_titan.get("container_sha256") == actual_titan_sha, "case Titan container SHA mismatch")
    require(case_titan.get("payload_sha256") == generation.get("payload_sha256"), "case Titan payload SHA mismatch")

    probe_ref = provenance.get("probe")
    require(isinstance(probe_ref, dict), "provenance probe reference is missing")
    actual_probe_sha = sha256_file(probe_path)
    require(hex64(probe_ref.get("sha256"), "profile probe SHA") == actual_probe_sha, "profile probe binary SHA-256 mismatch")
    require(probe_ref.get("return_code") == 0, "profile probe provenance return code is not zero")

    copy_ref = provenance.get("copy_output")
    require(isinstance(copy_ref, dict), "copy-output provenance is missing")
    actual_copy_sha = sha256_file(copy_output_path)
    require(hex64(copy_ref.get("sha256"), "copy-output SHA") == actual_copy_sha, "copy-output SHA-256 mismatch")
    require(copy_ref.get("valid_utf8") is True, "copy output was not valid UTF-8")
    require(integer(copy_ref.get("bytes"), "copy-output bytes") == copy_output_path.stat().st_size, "copy-output byte count mismatch")
    require(actual_copy_sha == generation.get("payload_sha256"), "copy output does not reproduce Titan logical payload")
    case_copy = case.get("raw", {}).get("copy") if isinstance(case.get("raw"), dict) else None
    require(isinstance(case_copy, dict), "case copy receipt is missing")
    require(case_copy.get("output_sha256") == actual_copy_sha, "case copy SHA differs from verified output")
    require(case_copy.get("output_valid_utf8") is True, "case copy UTF-8 verdict differs from verified output")

    identity = {
        "candidate_commit": commit,
        "candidate_tree": tree,
        "device_class": case.get("device_class"),
        "titan_report_sha256": actual_titan_report_sha,
        "container_sha256": actual_titan_sha,
        "payload_sha256": generation.get("payload_sha256"),
        "physical_host_receipt_sha256": actual_host_sha,
    }
    require(case.get("case_id") == canonical_sha256(identity), "case_id does not recompute from raw identity")

    pss_summary = validate_pss(provenance, case)
    phase_summary = validate_phase_binding(provenance, case)
    correctness = case.get("raw", {}).get("correctness") if isinstance(case.get("raw"), dict) else None
    require(isinstance(correctness, dict), "case correctness receipt is missing")
    require(correctness.get("data_loss_events") == 0, "verified case reports data loss")
    require(correctness.get("invalid_utf8_events") == 0, "verified case reports invalid UTF-8")
    require(correctness.get("crashes_or_ooms") == 0, "verified case reports crash/OOM")

    return {
        "schema": VERIFY_SCHEMA,
        "authority": VERIFY_AUTHORITY,
        "candidate_commit": commit,
        "candidate_tree": tree,
        "device_class": case.get("device_class"),
        "case_id": case.get("case_id"),
        "artifacts": {
            "provenance_sha256": sha256_bytes(provenance_raw),
            "case_sha256": actual_case_sha,
            "physical_host_sha256": actual_host_sha,
            "titan_report_sha256": actual_titan_report_sha,
            "titan_container_sha256": actual_titan_sha,
            "copy_output_sha256": actual_copy_sha,
            "probe_sha256": actual_probe_sha,
        },
        "physical_host": host_summary,
        "pss": pss_summary,
        "phases": phase_summary,
        "provenance_gate_passed": True,
    }


def write_exclusive(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    text = json.dumps(value, indent=2, sort_keys=True) + "\n"
    try:
        with path.open("x", encoding="utf-8", newline="\n") as handle:
            handle.write(text)
    except FileExistsError as exc:
        raise EvidenceInvalid(f"refusing to overwrite verification receipt: {path}") from exc


def main() -> int:
    parser = argparse.ArgumentParser(description="Independently verify one raw M8 physical profile case provenance bundle.")
    parser.add_argument("--provenance", type=Path, required=True)
    parser.add_argument("--case", type=Path, required=True)
    parser.add_argument("--physical-host", type=Path, required=True)
    parser.add_argument("--titan-report", type=Path, required=True)
    parser.add_argument("--titan", type=Path, required=True)
    parser.add_argument("--copy-output", type=Path, required=True)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        result = verify(
            provenance_path=args.provenance.resolve(),
            case_path=args.case.resolve(),
            host_path=args.physical_host.resolve(),
            titan_report_path=args.titan_report.resolve(),
            titan_path=args.titan.resolve(),
            copy_output_path=args.copy_output.resolve(),
            probe_path=args.probe.resolve(),
        )
        write_exclusive(args.output.resolve(), result)
        print(f"m8_profile_provenance_valid=true profile={result['device_class']} case_id={result['case_id']}")
        return 0
    except (EvidenceInvalid, OSError, ValueError) as exc:
        print(f"M8 profile provenance verification failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
