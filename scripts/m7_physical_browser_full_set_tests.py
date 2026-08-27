#!/usr/bin/env python3
from __future__ import annotations

import copy

from browser_competitor_benchmark_evidence import host_metadata, normalized_system_fingerprint
from m7_normalized_browser_full_set_tests import report as normalized_report
from m7_physical_browser_full_set import (
    PHYSICAL_BROWSER_FULL_SET_AUTHORITY,
    PhysicalBrowserFullSetInvalid,
    attach_physical_host_evidence,
    validate_physical_browser_full_set,
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def require_invalid(callable_, message: str) -> None:
    try:
        callable_()
    except PhysicalBrowserFullSetInvalid:
        return
    raise AssertionError(message)


def certified_host() -> dict[str, object]:
    host = copy.deepcopy(host_metadata())
    machine = host.get("benchmark_machine_metadata")
    if not isinstance(machine, dict):
        raise AssertionError("test host lacks M0 machine receipt")
    machine["physical_device_confirmed"] = True
    machine["run_label"] = "m7-physical-browser-full-set-test"
    machine["thermal"] = {
        "state": "nominal",
        "source": "test-fixture",
        "readings_c": [45.0, 46.0],
    }
    return host


def rebind_report(value: dict[str, object], host: dict[str, object]) -> None:
    fingerprint = normalized_system_fingerprint(host)
    value["host"] = copy.deepcopy(host)
    for case in value["browser_cases"]:
        case["host_platform"] = str(host["platform"])
        case["host_arch"] = str(host["arch"])
        case["system_fingerprint"] = fingerprint
        normalized = case.get("normalized_core_evidence")
        if isinstance(normalized, dict):
            normalized["host_platform"] = str(host["platform"])
            normalized["host_arch"] = str(host["arch"])
            normalized["system_fingerprint"] = fingerprint


def main() -> int:
    host = certified_host()
    base = normalized_report()
    rebind_report(base, host)

    physical = attach_physical_host_evidence(
        base,
        host_before=copy.deepcopy(host),
        host_after=copy.deepcopy(host),
    )
    validate_physical_browser_full_set(physical)
    require(
        physical["physical_browser_full_set_authority"]
        == PHYSICAL_BROWSER_FULL_SET_AUTHORITY,
        "physical browser full-set authority drifted",
    )
    require(
        physical["physical_host_evidence"]["before"]["checks"]["physical_metadata_complete"]
        is True,
        "pre-stage physical receipt was lost",
    )
    require(
        physical["physical_host_evidence"]["after"]["checks"]["physical_metadata_complete"]
        is True,
        "post-stage physical receipt was lost",
    )

    missing_pre_thermal = copy.deepcopy(host)
    missing_pre_thermal["benchmark_machine_metadata"]["thermal"] = {
        "state": "unknown",
        "source": "unavailable",
        "readings_c": [],
    }
    require_invalid(
        lambda: attach_physical_host_evidence(
            base,
            host_before=missing_pre_thermal,
            host_after=host,
        ),
        "browser full-set without pre-stage thermal observation was accepted",
    )

    missing_post_physical = copy.deepcopy(host)
    missing_post_physical["benchmark_machine_metadata"]["physical_device_confirmed"] = False
    require_invalid(
        lambda: attach_physical_host_evidence(
            base,
            host_before=host,
            host_after=missing_post_physical,
        ),
        "browser full-set without post-stage physical confirmation was accepted",
    )

    post_drift = copy.deepcopy(host)
    post_drift["cpu_model"] = "Different CPU"
    post_drift["benchmark_machine_metadata"]["cpu_model"] = "Different CPU"
    require_invalid(
        lambda: attach_physical_host_evidence(
            base,
            host_before=host,
            host_after=post_drift,
        ),
        "post-stage machine identity drift was accepted",
    )

    report_drift = copy.deepcopy(base)
    drift_host = copy.deepcopy(host)
    drift_host["physical_ram_mib"] = int(drift_host["physical_ram_mib"]) + 1024
    drift_host["benchmark_machine_metadata"]["physical_ram_mib"] = int(
        drift_host["benchmark_machine_metadata"]["physical_ram_mib"]
    ) + 1024
    drift_host["device_class"] = host["device_class"]
    drift_host["benchmark_machine_metadata"]["device_class"] = host["device_class"]
    report_drift["host"] = drift_host
    require_invalid(
        lambda: attach_physical_host_evidence(
            report_drift,
            host_before=host,
            host_after=host,
        ),
        "normalized report host drift was accepted",
    )

    forged = copy.deepcopy(physical)
    forged["physical_host_evidence"]["before"]["cpu_model"] = "forged"
    require_invalid(
        lambda: validate_physical_browser_full_set(forged),
        "forged embedded physical receipt was accepted",
    )

    wrong_authority = copy.deepcopy(physical)
    wrong_authority["physical_browser_full_set_authority"] = "legacy"
    require_invalid(
        lambda: validate_physical_browser_full_set(wrong_authority),
        "wrong physical browser authority was accepted",
    )

    print("M7 physical browser full-set authority tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
