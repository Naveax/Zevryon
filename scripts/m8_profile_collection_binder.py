#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import sys
from typing import Any

SOURCE_ROOT = Path(__file__).resolve().parents[1]
if str(SOURCE_ROOT) not in sys.path:
    sys.path.insert(0, str(SOURCE_ROOT))

from zevryon_platform.performance_contract import DEVICE_PROFILES, DeviceClass  # noqa: E402
from scripts.m8_profile_observation_gate import evaluate_document  # noqa: E402

CASE_SCHEMA = "zevryon.m8.profile-case.v1"
CASE_AUTHORITY = "m8-raw-profile-case-v1"
COLLECTION_SCHEMA = "zevryon.m8.profile-collection.v1"
COLLECTION_AUTHORITY = "m8-four-profile-collection-binder-v1"
OBSERVATION_SCHEMA = "zevryon.m8.profile-observations.v1"
TITAN_SCHEMA = "zevryon.m8.titan-fixture.v1"
TITAN_AUTHORITY = "m8-canonical-titan-fixture-v1"
HEX40 = re.compile(r"^[0-9a-f]{40}$")
HEX64 = re.compile(r"^[0-9a-f]{64}$")
SCROLL_SAMPLES = 257
MUTATION_SAMPLES = 257
MIB = 1024 * 1024
DECIMAL_MB = 1_000_000


class EvidenceInvalid(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise EvidenceInvalid(message)


def strict_json_bytes(raw: bytes, label: str) -> Any:
    try:
        text = raw.decode("utf-8", errors="strict")
    except UnicodeDecodeError as exc:
        raise EvidenceInvalid(f"{label} is not valid UTF-8") from exc
    try:
        return json.loads(
            text,
            parse_constant=lambda value: (_ for _ in ()).throw(
                EvidenceInvalid(f"{label} contains non-finite JSON constant: {value}")
            ),
        )
    except json.JSONDecodeError as exc:
        raise EvidenceInvalid(f"{label} is invalid JSON: {exc}") from exc


def sha256_bytes(raw: bytes) -> str:
    return hashlib.sha256(raw).hexdigest()


def write_exclusive(path: Path, value: object) -> bytes:
    path.parent.mkdir(parents=True, exist_ok=True)
    raw = (json.dumps(value, indent=2, sort_keys=True) + "\n").encode("utf-8")
    try:
        with path.open("xb") as handle:
            handle.write(raw)
    except FileExistsError as exc:
        raise EvidenceInvalid(f"refusing to overwrite output: {path}") from exc
    return raw


def exact_keys(value: Any, expected: set[str], label: str) -> dict[str, Any]:
    require(isinstance(value, dict), f"{label} must be an object")
    actual = set(value)
    missing = sorted(expected - actual)
    extra = sorted(actual - expected)
    require(not missing, f"{label} missing fields: {', '.join(missing)}")
    require(not extra, f"{label} contains unexpected fields: {', '.join(extra)}")
    return value


def number(value: Any, label: str, *, integer: bool = False) -> int | float:
    if integer:
        require(type(value) is int, f"{label} must be an integer")
        require(value >= 0, f"{label} must be non-negative")
        return value
    require(type(value) in (int, float), f"{label} must be numeric")
    output = float(value)
    require(math.isfinite(output), f"{label} must be finite")
    require(output >= 0.0, f"{label} must be non-negative")
    return output


def hex40(value: Any, label: str) -> str:
    require(isinstance(value, str) and HEX40.fullmatch(value) is not None, f"{label} must be lowercase 40-hex")
    return value


def hex64(value: Any, label: str) -> str:
    require(isinstance(value, str) and HEX64.fullmatch(value) is not None, f"{label} must be lowercase 64-hex")
    return value


def percentile_linear(samples: list[float], percentile: float) -> float:
    require(samples, "percentile sample set cannot be empty")
    ordered = sorted(samples)
    position = (len(ordered) - 1) * percentile
    lower = int(math.floor(position))
    upper = int(math.ceil(position))
    if lower == upper:
        return ordered[lower]
    fraction = position - lower
    return ordered[lower] + (ordered[upper] - ordered[lower]) * fraction


def load_titan(path: Path) -> tuple[dict[str, Any], bytes, str]:
    raw = path.read_bytes()
    document = strict_json_bytes(raw, "Titan report")
    exact_keys(
        document,
        {
            "schema",
            "authority",
            "mode",
            "candidate_commit",
            "candidate_tree",
            "corpus_path",
            "observed_envelope",
            "frozen_certification_envelope",
            "generation",
            "verification",
            "certification_threshold_met",
            "certification_eligible",
            "gate_passed",
        },
        "Titan report",
    )
    require(document["schema"] == TITAN_SCHEMA, "Titan report schema mismatch")
    require(document["authority"] == TITAN_AUTHORITY, "Titan authority mismatch")
    require(document["mode"] == "certification", "profile certification requires a certification-mode Titan report")
    require(document["certification_threshold_met"] is True, "Titan certification threshold is not met")
    require(document["certification_eligible"] is True, "Titan report is not certification eligible")
    require(document["gate_passed"] is True, "Titan report gate did not pass")
    hex40(document["candidate_commit"], "Titan candidate_commit")
    hex40(document["candidate_tree"], "Titan candidate_tree")

    envelope = exact_keys(
        document["observed_envelope"],
        {
            "logical_utf8_bytes",
            "logical_records",
            "logical_nodes",
            "style_runs",
            "resource_references",
            "largest_record_bytes",
            "largest_unbroken_token_bytes",
            "pathological_grapheme_bytes",
        },
        "Titan observed_envelope",
    )
    for name, value in envelope.items():
        number(value, f"Titan observed_envelope.{name}", integer=True)

    generation = exact_keys(
        document["generation"],
        {"container_sha256", "payload_sha256", "physical_bytes"},
        "Titan generation",
    )
    hex64(generation["container_sha256"], "Titan container_sha256")
    hex64(generation["payload_sha256"], "Titan payload_sha256")
    number(generation["physical_bytes"], "Titan physical_bytes", integer=True)
    return document, raw, sha256_bytes(raw)


def parse_case(
    path: Path,
    titan: dict[str, Any],
    titan_report_sha256: str,
) -> tuple[dict[str, Any], dict[str, Any], str]:
    raw_bytes = path.read_bytes()
    document = strict_json_bytes(raw_bytes, f"profile case {path}")
    exact_keys(
        document,
        {
            "schema",
            "authority",
            "case_id",
            "candidate_commit",
            "candidate_tree",
            "device_class",
            "titan",
            "physical_host",
            "runtime_policy",
            "raw",
        },
        f"profile case {path}",
    )
    require(document["schema"] == CASE_SCHEMA, f"profile case {path} schema mismatch")
    require(document["authority"] == CASE_AUTHORITY, f"profile case {path} authority mismatch")
    require(isinstance(document["case_id"], str) and document["case_id"].strip(), "case_id must be non-empty")
    commit = hex40(document["candidate_commit"], "case candidate_commit")
    tree = hex40(document["candidate_tree"], "case candidate_tree")
    require(commit == titan["candidate_commit"], "case candidate_commit differs from Titan candidate")
    require(tree == titan["candidate_tree"], "case candidate_tree differs from Titan candidate")

    try:
        device = DeviceClass(document["device_class"])
    except (TypeError, ValueError) as exc:
        raise EvidenceInvalid(f"invalid device_class: {document.get('device_class')!r}") from exc
    profile = DEVICE_PROFILES[device]

    titan_ref = exact_keys(
        document["titan"],
        {"report_sha256", "container_sha256", "payload_sha256"},
        "case titan",
    )
    require(hex64(titan_ref["report_sha256"], "case titan report_sha256") == titan_report_sha256, "case Titan report SHA-256 mismatch")
    require(hex64(titan_ref["container_sha256"], "case titan container_sha256") == titan["generation"]["container_sha256"], "case Titan container SHA-256 mismatch")
    require(hex64(titan_ref["payload_sha256"], "case titan payload_sha256") == titan["generation"]["payload_sha256"], "case Titan payload SHA-256 mismatch")

    host = exact_keys(
        document["physical_host"],
        {
            "authority",
            "receipt_sha256",
            "qualified",
            "device_class",
            "physical_memory_mib",
            "process_group_complete",
            "pss_authority",
        },
        "physical_host",
    )
    require(isinstance(host["authority"], str) and host["authority"], "physical_host.authority must be non-empty")
    hex64(host["receipt_sha256"], "physical_host.receipt_sha256")
    require(host["qualified"] is True, "physical host is not qualified")
    require(host["device_class"] == device.value, "physical host device_class mismatch")
    physical_memory = number(host["physical_memory_mib"], "physical_host.physical_memory_mib", integer=True)
    require(physical_memory >= profile.minimum_physical_ram_mib, "physical host RAM is below the profile minimum")
    require(host["process_group_complete"] is True, "physical host process-group ownership is incomplete")
    require(host["pss_authority"] == "aggregate-pss", "profile certification requires aggregate PSS authority")

    policy = exact_keys(
        document["runtime_policy"],
        {
            "profile",
            "within_profile_budgets",
            "hot_budget_bytes",
            "hot_allocated_bytes",
            "warm_budget_bytes",
            "warm_allocated_bytes",
            "cold_budget_bytes",
            "cold_allocated_bytes",
        },
        "runtime_policy",
    )
    require(policy["profile"] == device.value, "runtime policy profile mismatch")
    require(policy["within_profile_budgets"] is True, "runtime policy reports an out-of-budget configuration")
    hot_budget = number(policy["hot_budget_bytes"], "runtime_policy.hot_budget_bytes", integer=True)
    hot_allocated = number(policy["hot_allocated_bytes"], "runtime_policy.hot_allocated_bytes", integer=True)
    warm_budget = number(policy["warm_budget_bytes"], "runtime_policy.warm_budget_bytes", integer=True)
    warm_allocated = number(policy["warm_allocated_bytes"], "runtime_policy.warm_allocated_bytes", integer=True)
    cold_budget = number(policy["cold_budget_bytes"], "runtime_policy.cold_budget_bytes", integer=True)
    cold_allocated = number(policy["cold_allocated_bytes"], "runtime_policy.cold_allocated_bytes", integer=True)
    require(hot_budget == profile.hot_cache_mb * DECIMAL_MB, "runtime hot budget differs from frozen profile")
    require(warm_budget == profile.warm_cache_mb * DECIMAL_MB, "runtime warm budget differs from frozen profile")
    require(cold_budget == profile.cold_cache_mb * DECIMAL_MB, "runtime cold budget differs from frozen profile")
    require(hot_allocated <= hot_budget, "runtime hot allocation exceeds profile budget")
    require(warm_allocated <= warm_budget, "runtime warm allocation exceeds profile budget")
    require(cold_allocated <= cold_budget, "runtime cold allocation exceeds profile budget")

    raw = exact_keys(
        document["raw"],
        {
            "pss_samples_mb",
            "first_viewport_streaming_ms",
            "first_viewport_preindexed_ms",
            "scroll_samples_ms",
            "exact_search_cold_ms",
            "exact_search_warm_ms",
            "mutation_samples_us",
            "copy",
            "correctness",
            "probe_return_code",
            "probe_terminated_abnormally",
        },
        "raw",
    )
    pss_samples_raw = raw["pss_samples_mb"]
    require(isinstance(pss_samples_raw, list) and pss_samples_raw, "pss_samples_mb must be a non-empty array")
    pss_samples = [float(number(item, "pss sample")) for item in pss_samples_raw]

    streaming_ms = float(number(raw["first_viewport_streaming_ms"], "first_viewport_streaming_ms"))
    preindexed_ms = float(number(raw["first_viewport_preindexed_ms"], "first_viewport_preindexed_ms"))

    scroll_raw = raw["scroll_samples_ms"]
    require(isinstance(scroll_raw, list) and len(scroll_raw) == SCROLL_SAMPLES, f"scroll_samples_ms must contain exactly {SCROLL_SAMPLES} samples")
    scroll_samples = [float(number(item, "scroll sample")) for item in scroll_raw]

    cold_ms = float(number(raw["exact_search_cold_ms"], "exact_search_cold_ms"))
    warm_ms = float(number(raw["exact_search_warm_ms"], "exact_search_warm_ms"))

    mutation_raw = raw["mutation_samples_us"]
    require(isinstance(mutation_raw, list) and len(mutation_raw) == MUTATION_SAMPLES, f"mutation_samples_us must contain exactly {MUTATION_SAMPLES} samples")
    mutation_samples = [float(number(item, "mutation sample")) for item in mutation_raw]

    copy = exact_keys(
        raw["copy"],
        {"source_bytes", "output_bytes", "elapsed_seconds", "output_sha256", "output_valid_utf8", "cancelled"},
        "copy",
    )
    source_bytes = number(copy["source_bytes"], "copy.source_bytes", integer=True)
    output_bytes = number(copy["output_bytes"], "copy.output_bytes", integer=True)
    elapsed_seconds = float(number(copy["elapsed_seconds"], "copy.elapsed_seconds"))
    require(elapsed_seconds > 0.0, "copy.elapsed_seconds must be positive")
    output_sha256 = hex64(copy["output_sha256"], "copy.output_sha256")
    require(type(copy["output_valid_utf8"]) is bool, "copy.output_valid_utf8 must be boolean")
    require(type(copy["cancelled"]) is bool, "copy.cancelled must be boolean")

    correctness = exact_keys(
        raw["correctness"],
        {"data_loss_events", "invalid_utf8_events", "crashes_or_ooms"},
        "correctness",
    )
    explicit_data_loss = number(correctness["data_loss_events"], "correctness.data_loss_events", integer=True)
    explicit_invalid_utf8 = number(correctness["invalid_utf8_events"], "correctness.invalid_utf8_events", integer=True)
    explicit_crashes = number(correctness["crashes_or_ooms"], "correctness.crashes_or_ooms", integer=True)
    require(type(raw["probe_return_code"]) is int, "probe_return_code must be an integer")
    require(type(raw["probe_terminated_abnormally"]) is bool, "probe_terminated_abnormally must be boolean")

    envelope = titan["observed_envelope"]
    payload_bytes = int(envelope["logical_utf8_bytes"])
    copy_mismatch = (
        source_bytes != payload_bytes
        or output_bytes != payload_bytes
        or output_sha256 != titan["generation"]["payload_sha256"]
        or bool(copy["cancelled"])
    )
    data_loss_events = int(explicit_data_loss) + (1 if copy_mismatch else 0)
    invalid_utf8_events = int(explicit_invalid_utf8) + (0 if copy["output_valid_utf8"] else 1)
    crashes_or_ooms = int(explicit_crashes) + (1 if raw["probe_terminated_abnormally"] or raw["probe_return_code"] != 0 else 0)

    observation = {
        "device_class": device.value,
        **{name: int(value) for name, value in envelope.items()},
        "process_group_pss_mb": max(pss_samples),
        "first_viewport_preindexed_ms": preindexed_ms,
        "first_viewport_streaming_ms": streaming_ms,
        "scroll_p99_ms": percentile_linear(scroll_samples, 0.99),
        "maximum_normal_stall_ms": max(scroll_samples),
        "exact_search_warm_ms": warm_ms,
        "exact_search_cold_ms": cold_ms,
        "mutation_p95_us": percentile_linear(mutation_samples, 0.95),
        "copy_throughput_mib_s": (float(source_bytes) / MIB) / elapsed_seconds,
        "data_loss_events": data_loss_events,
        "invalid_utf8_events": invalid_utf8_events,
        "crashes_or_ooms": crashes_or_ooms,
    }

    receipt = {
        "case_id": document["case_id"],
        "device_class": device.value,
        "case_path": str(path.resolve()),
        "case_sha256": sha256_bytes(raw_bytes),
        "physical_host_receipt_sha256": host["receipt_sha256"],
        "derived_observation": observation,
    }
    return observation, receipt, document["case_id"]


def bind(
    titan_path: Path,
    case_paths: list[Path],
) -> tuple[dict[str, Any], dict[str, Any], bool]:
    require(len(case_paths) == len(DeviceClass), "exactly four profile case artifacts are required")
    titan, titan_raw, titan_sha = load_titan(titan_path)

    observations: list[dict[str, Any]] = []
    receipts: list[dict[str, Any]] = []
    case_ids: set[str] = set()
    devices: set[str] = set()
    for path in case_paths:
        observation, receipt, case_id = parse_case(path, titan, titan_sha)
        require(case_id not in case_ids, "profile case_id values must be unique")
        require(observation["device_class"] not in devices, "profile device classes must be unique")
        case_ids.add(case_id)
        devices.add(observation["device_class"])
        observations.append(observation)
        receipts.append(receipt)

    required_devices = {device.value for device in DeviceClass}
    require(devices == required_devices, "profile case set is incomplete")
    observations.sort(key=lambda item: item["device_class"])
    receipts.sort(key=lambda item: item["device_class"])

    observation_document = {
        "schema": OBSERVATION_SCHEMA,
        "candidate_commit": titan["candidate_commit"],
        "candidate_tree": titan["candidate_tree"],
        "observations": observations,
    }
    observation_raw = (json.dumps(observation_document, indent=2, sort_keys=True) + "\n").encode("utf-8")
    _, gate_passed = evaluate_document(observation_document, observation_raw)

    collection = {
        "schema": COLLECTION_SCHEMA,
        "authority": COLLECTION_AUTHORITY,
        "candidate_commit": titan["candidate_commit"],
        "candidate_tree": titan["candidate_tree"],
        "titan_report_path": str(titan_path.resolve()),
        "titan_report_sha256": sha256_bytes(titan_raw),
        "titan_container_sha256": titan["generation"]["container_sha256"],
        "titan_payload_sha256": titan["generation"]["payload_sha256"],
        "required_device_classes": sorted(required_devices),
        "cases": receipts,
        "observations_sha256": sha256_bytes(observation_raw),
        "recomputed_four_profile_gate_passed": gate_passed,
        "evidence_valid": True,
    }
    return observation_document, collection, gate_passed


def main() -> int:
    parser = argparse.ArgumentParser(description="Bind four independently collected M8 physical profile cases into one no-compensation observation artifact.")
    parser.add_argument("--titan-report", type=Path, required=True)
    parser.add_argument("--case", dest="cases", action="append", type=Path, required=True)
    parser.add_argument("--observations-output", type=Path, required=True)
    parser.add_argument("--receipt-output", type=Path, required=True)
    args = parser.parse_args()

    try:
        require(args.observations_output.resolve() != args.receipt_output.resolve(), "binder outputs must differ")
        observations, collection, gate_passed = bind(args.titan_report, args.cases)
        observation_raw = write_exclusive(args.observations_output, observations)
        require(collection["observations_sha256"] == sha256_bytes(observation_raw), "observation serialization SHA-256 drifted")
        write_exclusive(args.receipt_output, collection)
    except (EvidenceInvalid, OSError) as exc:
        print(f"M8 profile collection binder failed: {exc}", file=sys.stderr)
        return 1
    return 0 if gate_passed else 2


if __name__ == "__main__":
    raise SystemExit(main())
