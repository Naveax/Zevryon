#!/usr/bin/env python3
from __future__ import annotations

import copy

from browser_competitor_benchmark_evidence import (
    HARNESS_SCHEMA,
    SYSTEM_FINGERPRINT_SCHEMA,
    normalized_system_fingerprint,
)
from m7_zevryon_physical_case import (
    PHYSICAL_CASE_AUTHORITY,
    PhysicalZevryonCaseInvalid,
    attach_physical_host_evidence,
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def require_invalid(callable_, message: str) -> None:
    try:
        callable_()
    except PhysicalZevryonCaseInvalid:
        return
    raise AssertionError(message)


def host_fixture(timestamp: str = "2026-08-28T00:00:00Z") -> dict[str, object]:
    machine = {
        "schema_version": 1,
        "captured_at_utc": timestamp,
        "device_class": "desktop",
        "physical_device_confirmed": True,
        "physical_ram_mib": 32768,
        "logical_cpu_count": 16,
        "os_name": "TestOS",
        "os_release": "1.2.3-test",
        "architecture": "x86_64",
        "cpu_model": "Test CPU 8-Core",
        "run_label": "m7-canonical-test",
        "thermal": {
            "state": "nominal",
            "source": "test-fixture",
            "readings_c": [48.5, 49.0],
        },
    }
    return {
        "system_fingerprint_schema": SYSTEM_FINGERPRINT_SCHEMA,
        "machine_metadata_schema": 1,
        "platform": "TestOS",
        "arch": "x86_64",
        "kernel": "1.2.3-test",
        "logical_cpus": 16,
        "cpu_model": "Test CPU 8-Core",
        "physical_ram_mib": 32768,
        "device_class": "desktop",
        "benchmark_machine_metadata": machine,
    }


def record_fixture(host: dict[str, object]) -> dict[str, object]:
    return {
        "schema": "zevryon.massivedoc.normalized-case.v1",
        "implementation": "zevryon",
        "status": "success",
        "mode": "virtualized",
        "host_platform": host["platform"],
        "host_arch": host["arch"],
        "system_fingerprint": normalized_system_fingerprint(host),
        "harness_schema": HARNESS_SCHEMA,
        "corpus_sha256": "a" * 64,
        "scenario_fingerprint": "b" * 64,
    }


def main() -> int:
    before = host_fixture()
    after = host_fixture("2026-08-28T00:02:00Z")
    record = record_fixture(before)

    attached = attach_physical_host_evidence(
        record,
        host_before=before,
        host_after=after,
    )
    require(
        attached["physical_case_authority"] == PHYSICAL_CASE_AUTHORITY,
        "physical case authority drifted",
    )
    require(attached["host"] == before, "pre-case raw host receipt was lost")
    require(attached["host_after"] == after, "post-case raw host receipt was lost")
    require(
        attached["physical_host_evidence"]["physical_host_gate_passed"] is True,
        "physical host gate did not pass",
    )
    require(
        attached["physical_host_evidence"]["before"]["checks"]["physical_metadata_complete"]
        is True,
        "pre-case M0 physical certification was lost",
    )
    require(
        attached["physical_host_evidence"]["after"]["checks"]["physical_metadata_complete"]
        is True,
        "post-case M0 physical certification was lost",
    )

    unconfirmed = copy.deepcopy(before)
    unconfirmed["benchmark_machine_metadata"]["physical_device_confirmed"] = False
    require_invalid(
        lambda: attach_physical_host_evidence(
            record,
            host_before=unconfirmed,
            host_after=after,
        ),
        "unconfirmed Zevryon physical host was accepted",
    )

    no_thermal = copy.deepcopy(after)
    no_thermal["benchmark_machine_metadata"]["thermal"] = {
        "state": "unknown",
        "source": "unavailable",
        "readings_c": [],
    }
    require_invalid(
        lambda: attach_physical_host_evidence(
            record,
            host_before=before,
            host_after=no_thermal,
        ),
        "Zevryon case without post-case thermal evidence was accepted",
    )

    different_machine = copy.deepcopy(after)
    different_machine["physical_ram_mib"] = 65536
    different_machine["benchmark_machine_metadata"]["physical_ram_mib"] = 65536
    require_invalid(
        lambda: attach_physical_host_evidence(
            record,
            host_before=before,
            host_after=different_machine,
        ),
        "post-case machine fingerprint drift was accepted",
    )

    platform_drift = copy.deepcopy(record)
    platform_drift["host_platform"] = "AnotherOS"
    require_invalid(
        lambda: attach_physical_host_evidence(
            platform_drift,
            host_before=before,
            host_after=after,
        ),
        "record/physical-host platform drift was accepted",
    )

    missing_fingerprint = copy.deepcopy(record)
    missing_fingerprint.pop("system_fingerprint")
    require_invalid(
        lambda: attach_physical_host_evidence(
            missing_fingerprint,
            host_before=before,
            host_after=after,
        ),
        "record without system fingerprint was accepted",
    )

    print("M7 Zevryon physical-case authority tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
