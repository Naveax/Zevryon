#!/usr/bin/env python3
from __future__ import annotations

import copy

from browser_competitor_registry import CANONICAL_KEYS, get_spec
from m7_runtime_preflight import (
    PREFLIGHT_AUTHORITY,
    PREFLIGHT_SCHEMA,
    RuntimePreflightInvalid,
    run_runtime_preflight,
    validate_runtime_preflight_report,
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def require_invalid(callable_, message: str) -> None:
    try:
        callable_()
    except RuntimePreflightInvalid:
        return
    raise AssertionError(message)


def success_probe(competitor: str) -> dict[str, object]:
    spec = get_spec(competitor)
    return {
        "competitor": competitor,
        "canonical_name": spec.canonical_name,
        "adapter": spec.adapter,
        "status": "success",
        "launch_ready": True,
        "runtime_identity": f"{competitor}|exact-test-runtime",
        "reason": None,
    }


def webdriver_success_probe(competitor: str) -> dict[str, object]:
    return success_probe(competitor)


def main() -> int:
    valid = run_runtime_preflight(
        playwright_probe=success_probe,
        webdriver_probe=webdriver_success_probe,
    )
    require(valid["schema"] == PREFLIGHT_SCHEMA, "preflight schema drifted")
    require(
        valid["preflight_authority"] == PREFLIGHT_AUTHORITY,
        "preflight authority drifted",
    )
    require(
        isinstance(valid.get("host"), dict),
        "preflight host metadata is missing",
    )
    require(
        isinstance(valid.get("system_fingerprint"), str)
        and len(valid["system_fingerprint"]) == 64,
        "preflight system fingerprint is missing",
    )
    require(valid["all_runtimes_ready"] is True, "valid runtimes were not ready")
    require(valid["preflight_gate_passed"] is True, "valid preflight gate did not pass")
    require(valid["measurement_started"] is False, "preflight claimed measurement")
    require(
        [record["competitor"] for record in valid["records"]] == list(CANONICAL_KEYS),
        "preflight canonical order drifted",
    )

    def missing_servo(competitor: str) -> dict[str, object]:
        if competitor != "servo":
            return success_probe(competitor)
        spec = get_spec(competitor)
        return {
            "competitor": competitor,
            "canonical_name": spec.canonical_name,
            "adapter": spec.adapter,
            "status": "unavailable",
            "launch_ready": False,
            "runtime_identity": None,
            "reason": "Servo binary unavailable",
        }

    partial = run_runtime_preflight(
        playwright_probe=success_probe,
        webdriver_probe=missing_servo,
    )
    require(partial["all_runtimes_ready"] is False, "missing Servo was hidden")
    require(partial["preflight_gate_passed"] is False, "partial preflight passed")
    servo = next(record for record in partial["records"] if record["competitor"] == "servo")
    require(servo["status"] == "unavailable", "Servo failure state drifted")

    forged_system = copy.deepcopy(valid)
    forged_system["system_fingerprint"] = "0" * 64
    require_invalid(
        lambda: validate_runtime_preflight_report(forged_system),
        "forged preflight system fingerprint was accepted",
    )

    malformed_host = copy.deepcopy(valid)
    malformed_host["host"] = {"platform": "", "arch": "", "kernel": "", "logical_cpus": 0}
    require_invalid(
        lambda: validate_runtime_preflight_report(malformed_host),
        "malformed preflight host metadata was accepted",
    )

    wrong_set = copy.deepcopy(valid)
    wrong_set["canonical_competitors"].pop()
    require_invalid(
        lambda: validate_runtime_preflight_report(wrong_set),
        "partial canonical set was accepted",
    )

    duplicate = copy.deepcopy(valid)
    duplicate["records"][-1] = copy.deepcopy(duplicate["records"][0])
    require_invalid(
        lambda: validate_runtime_preflight_report(duplicate),
        "duplicate canonical runtime was accepted",
    )

    missing_identity = copy.deepcopy(valid)
    missing_identity["records"][0]["runtime_identity"] = None
    require_invalid(
        lambda: validate_runtime_preflight_report(missing_identity),
        "successful runtime without identity was accepted",
    )

    success_with_reason = copy.deepcopy(valid)
    success_with_reason["records"][0]["reason"] = "should not exist"
    require_invalid(
        lambda: validate_runtime_preflight_report(success_with_reason),
        "successful runtime with failure reason was accepted",
    )

    failure_with_identity = copy.deepcopy(partial)
    servo_failure = next(
        record for record in failure_with_identity["records"]
        if record["competitor"] == "servo"
    )
    servo_failure["runtime_identity"] = "servo|forged"
    require_invalid(
        lambda: validate_runtime_preflight_report(failure_with_identity),
        "failed runtime with admitted identity was accepted",
    )

    forged_summary = copy.deepcopy(partial)
    forged_summary["all_runtimes_ready"] = True
    require_invalid(
        lambda: validate_runtime_preflight_report(forged_summary),
        "forged all-ready summary was accepted",
    )

    forged_gate = copy.deepcopy(partial)
    forged_gate["preflight_gate_passed"] = True
    require_invalid(
        lambda: validate_runtime_preflight_report(forged_gate),
        "forged preflight gate was accepted",
    )

    measurement_claim = copy.deepcopy(valid)
    measurement_claim["measurement_started"] = True
    require_invalid(
        lambda: validate_runtime_preflight_report(measurement_claim),
        "preflight benchmark-measurement claim was accepted",
    )

    wrong_adapter = copy.deepcopy(valid)
    wrong_adapter["records"][0]["adapter"] = "webdriver"
    require_invalid(
        lambda: validate_runtime_preflight_report(wrong_adapter),
        "runtime adapter drift was accepted",
    )

    print("M7 canonical runtime preflight authority tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
