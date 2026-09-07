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

from zevryon_platform.performance_contract import (  # noqa: E402
    BenchmarkObservation,
    DeviceClass,
    evaluate,
)

INPUT_SCHEMA = "zevryon.m8.profile-observations.v1"
OUTPUT_SCHEMA = "zevryon.m8.profile-gate.v1"
HEX40 = re.compile(r"^[0-9a-f]{40}$")

OBSERVATION_FIELDS = (
    "device_class",
    "logical_utf8_bytes",
    "logical_records",
    "logical_nodes",
    "style_runs",
    "resource_references",
    "largest_record_bytes",
    "largest_unbroken_token_bytes",
    "pathological_grapheme_bytes",
    "process_group_pss_mb",
    "first_viewport_preindexed_ms",
    "first_viewport_streaming_ms",
    "scroll_p99_ms",
    "maximum_normal_stall_ms",
    "exact_search_warm_ms",
    "exact_search_cold_ms",
    "mutation_p95_us",
    "copy_throughput_mib_s",
    "data_loss_events",
    "invalid_utf8_events",
    "crashes_or_ooms",
)

INTEGER_FIELDS = {
    "logical_utf8_bytes",
    "logical_records",
    "logical_nodes",
    "style_runs",
    "resource_references",
    "largest_record_bytes",
    "largest_unbroken_token_bytes",
    "pathological_grapheme_bytes",
    "data_loss_events",
    "invalid_utf8_events",
    "crashes_or_ooms",
}


class EvidenceInvalid(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise EvidenceInvalid(message)


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def require_number(name: str, value: Any, integer: bool) -> int | float:
    if integer:
        require(type(value) is int, f"{name} must be an integer")
        require(value >= 0, f"{name} must be non-negative")
        return value
    require(type(value) in (int, float), f"{name} must be numeric")
    numeric = float(value)
    require(math.isfinite(numeric), f"{name} must be finite")
    require(numeric >= 0.0, f"{name} must be non-negative")
    return numeric


def parse_observation(raw: Any) -> BenchmarkObservation:
    require(isinstance(raw, dict), "each observation must be an object")
    expected = set(OBSERVATION_FIELDS)
    actual = set(raw)
    missing = sorted(expected - actual)
    extra = sorted(actual - expected)
    require(not missing, "observation missing fields: " + ", ".join(missing))
    require(not extra, "observation contains non-raw fields: " + ", ".join(extra))

    try:
        device = DeviceClass(raw["device_class"])
    except (TypeError, ValueError) as exc:
        raise EvidenceInvalid(f"invalid device_class: {raw.get('device_class')!r}") from exc

    values: dict[str, Any] = {"device_class": device}
    for field in OBSERVATION_FIELDS:
        if field == "device_class":
            continue
        values[field] = require_number(field, raw[field], field in INTEGER_FIELDS)
    return BenchmarkObservation(**values)


def evaluate_document(document: Any, raw_bytes: bytes) -> tuple[dict[str, Any], bool]:
    require(isinstance(document, dict), "profile evidence must be a JSON object")
    require(document.get("schema") == INPUT_SCHEMA, "profile evidence schema mismatch")

    commit = document.get("candidate_commit")
    tree = document.get("candidate_tree")
    require(isinstance(commit, str) and HEX40.fullmatch(commit) is not None, "candidate_commit must be lowercase 40-hex")
    require(isinstance(tree, str) and HEX40.fullmatch(tree) is not None, "candidate_tree must be lowercase 40-hex")

    raw_observations = document.get("observations")
    require(isinstance(raw_observations, list), "observations must be an array")
    require(len(raw_observations) == len(DeviceClass), "exactly four device observations are required")

    observations = [parse_observation(item) for item in raw_observations]
    devices = [item.device_class for item in observations]
    require(len(set(devices)) == len(devices), "device observations contain duplicates")
    require(set(devices) == set(DeviceClass), "device observation set is incomplete")

    results: list[dict[str, Any]] = []
    all_passed = True
    for observation in sorted(observations, key=lambda item: item.device_class.value):
        checks = evaluate(observation)
        passed = checks["score_100"]
        all_passed = all_passed and passed
        results.append(
            {
                "device_class": observation.device_class.value,
                "checks": checks,
                "score_100": passed,
            }
        )

    report = {
        "schema": OUTPUT_SCHEMA,
        "authority": "m8-four-profile-no-compensation-v1",
        "candidate_commit": commit,
        "candidate_tree": tree,
        "input_sha256": sha256_bytes(raw_bytes),
        "required_device_classes": sorted(device.value for device in DeviceClass),
        "results": results,
        "all_profiles_score_100": all_passed,
        "gate_passed": all_passed,
    }
    return report, all_passed


def load_json_strict(raw_bytes: bytes) -> Any:
    try:
        text = raw_bytes.decode("utf-8", errors="strict")
    except UnicodeDecodeError as exc:
        raise EvidenceInvalid("profile evidence is not valid UTF-8") from exc
    try:
        return json.loads(
            text,
            parse_constant=lambda value: (_ for _ in ()).throw(
                EvidenceInvalid(f"non-finite JSON constant is forbidden: {value}")
            ),
        )
    except json.JSONDecodeError as exc:
        raise EvidenceInvalid(f"profile evidence is invalid JSON: {exc}") from exc


def main() -> int:
    parser = argparse.ArgumentParser(description="Recompute the M8 no-compensation gate for all four device profiles from raw observations.")
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    try:
        raw_bytes = args.input.read_bytes()
        document = load_json_strict(raw_bytes)
        report, passed = evaluate_document(document, raw_bytes)
    except (EvidenceInvalid, OSError) as exc:
        report = {
            "schema": OUTPUT_SCHEMA,
            "authority": "m8-four-profile-no-compensation-v1",
            "gate_passed": False,
            "evidence_valid": False,
            "error": str(exc),
        }
        exit_code = 1
    else:
        report["evidence_valid"] = True
        exit_code = 0 if passed else 2

    text = json.dumps(report, indent=2, sort_keys=True) + "\n"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(text, encoding="utf-8")
    print(text, end="")
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
