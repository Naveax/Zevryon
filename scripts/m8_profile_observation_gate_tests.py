#!/usr/bin/env python3
from __future__ import annotations

import json

import m8_profile_observation_gate as gate

from zevryon_platform.performance_contract import (
    DEVICE_PROFILES,
    TITAN_WORST_CASE,
    DeviceClass,
)

COMMIT = "1" * 40
TREE = "2" * 40


def observation_for(device: DeviceClass) -> dict[str, object]:
    profile = DEVICE_PROFILES[device]
    titan = TITAN_WORST_CASE
    return {
        "device_class": device.value,
        "logical_utf8_bytes": titan.logical_utf8_bytes,
        "logical_records": titan.logical_records,
        "logical_nodes": titan.logical_nodes,
        "style_runs": titan.style_runs,
        "resource_references": titan.resource_references,
        "largest_record_bytes": titan.largest_record_bytes,
        "largest_unbroken_token_bytes": titan.largest_unbroken_token_bytes,
        "pathological_grapheme_bytes": titan.pathological_grapheme_bytes,
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


def document() -> dict[str, object]:
    return {
        "schema": gate.INPUT_SCHEMA,
        "candidate_commit": COMMIT,
        "candidate_tree": TREE,
        "observations": [observation_for(device) for device in DeviceClass],
    }


def raw(value: object) -> bytes:
    return (json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode("utf-8")


def expect_invalid(value: object, expected_fragment: str) -> None:
    try:
        gate.evaluate_document(value, raw(value))
    except gate.EvidenceInvalid as exc:
        assert expected_fragment in str(exc), (expected_fragment, str(exc))
        return
    raise AssertionError(f"expected invalid evidence containing {expected_fragment!r}")


def test_all_four_profiles_pass_only_when_each_scores_100() -> None:
    value = document()
    report, passed = gate.evaluate_document(value, raw(value))
    assert passed
    assert report["gate_passed"] is True
    assert report["all_profiles_score_100"] is True
    assert len(report["results"]) == 4
    assert all(item["score_100"] is True for item in report["results"])


def test_one_profile_failure_cannot_be_compensated() -> None:
    value = document()
    observations = value["observations"]
    assert isinstance(observations, list)
    legacy = next(item for item in observations if item["device_class"] == DeviceClass.LEGACY_PHONE.value)
    legacy["process_group_pss_mb"] = DEVICE_PROFILES[DeviceClass.LEGACY_PHONE].process_group_pss_target_mb + 0.01
    report, passed = gate.evaluate_document(value, raw(value))
    assert not passed
    assert report["gate_passed"] is False
    legacy_result = next(item for item in report["results"] if item["device_class"] == DeviceClass.LEGACY_PHONE.value)
    assert legacy_result["checks"]["memory_target"] is False
    assert legacy_result["checks"]["memory_hard_cap"] is True


def test_missing_or_duplicate_profile_is_invalid_evidence() -> None:
    missing = document()
    observations = missing["observations"]
    assert isinstance(observations, list)
    observations.pop()
    expect_invalid(missing, "exactly four")

    duplicate = document()
    duplicate_observations = duplicate["observations"]
    assert isinstance(duplicate_observations, list)
    duplicate_observations[-1]["device_class"] = duplicate_observations[0]["device_class"]
    expect_invalid(duplicate, "duplicates")


def test_hand_authored_score_field_is_rejected() -> None:
    value = document()
    observations = value["observations"]
    assert isinstance(observations, list)
    observations[0]["score_100"] = True
    expect_invalid(value, "non-raw fields")


def test_adversarial_titan_dimension_is_recomputed() -> None:
    value = document()
    observations = value["observations"]
    assert isinstance(observations, list)
    observations[0]["pathological_grapheme_bytes"] = TITAN_WORST_CASE.pathological_grapheme_bytes - 1
    report, passed = gate.evaluate_document(value, raw(value))
    assert not passed
    first = next(item for item in report["results"] if item["device_class"] == observations[0]["device_class"])
    assert first["checks"]["certified_pathological_grapheme"] is False


def test_nonfinite_json_is_invalid() -> None:
    try:
        gate.load_json_strict(b'{"schema":"x","value":NaN}')
    except gate.EvidenceInvalid as exc:
        assert "non-finite" in str(exc)
        return
    raise AssertionError("NaN was accepted as raw evidence")


def main() -> int:
    test_all_four_profiles_pass_only_when_each_scores_100()
    test_one_profile_failure_cannot_be_compensated()
    test_missing_or_duplicate_profile_is_invalid_evidence()
    test_hand_authored_score_field_is_rejected()
    test_adversarial_titan_dimension_is_recomputed()
    test_nonfinite_json_is_invalid()
    print("Zevryon M8 four-profile observation gate tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
