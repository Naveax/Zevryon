#!/usr/bin/env python3
from __future__ import annotations

import copy

from browser_competitor_benchmark_evidence import (
    SYSTEM_FINGERPRINT_SCHEMA,
    normalized_system_fingerprint,
)
from m7_physical_host_evidence import (
    PHYSICAL_HOST_AUTHORITY,
    PhysicalHostEvidenceInvalid,
    certify_physical_host,
    machine_metadata_from_host,
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def require_invalid(callable_, message: str) -> None:
    try:
        callable_()
    except PhysicalHostEvidenceInvalid:
        return
    raise AssertionError(message)


def host_fixture() -> dict[str, object]:
    machine = {
        "schema_version": 1,
        "captured_at_utc": "2026-08-28T00:00:00Z",
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


def main() -> int:
    host = host_fixture()
    metadata = machine_metadata_from_host(host)
    require(metadata.physical_device_confirmed, "physical confirmation was lost")
    require(metadata.cpu_model == "Test CPU 8-Core", "CPU model drifted")
    require(
        normalized_system_fingerprint(host),
        "stable system fingerprint was not produced",
    )

    receipt = certify_physical_host(host, label="test-host")
    require(receipt["authority"] == PHYSICAL_HOST_AUTHORITY, "authority drifted")
    require(receipt["physical_host_gate_passed"] is True, "physical gate did not pass")
    require(receipt["checks"]["physical_metadata_complete"] is True, "M0 checks drifted")

    unconfirmed = copy.deepcopy(host)
    unconfirmed["benchmark_machine_metadata"]["physical_device_confirmed"] = False
    require_invalid(
        lambda: certify_physical_host(unconfirmed, label="unconfirmed"),
        "unconfirmed host was certified",
    )

    no_thermal = copy.deepcopy(host)
    no_thermal["benchmark_machine_metadata"]["thermal"] = {
        "state": "unknown",
        "source": "unavailable",
        "readings_c": [],
    }
    require_invalid(
        lambda: certify_physical_host(no_thermal, label="no-thermal"),
        "host without thermal observation was certified",
    )

    drifted_cpu = copy.deepcopy(host)
    drifted_cpu["cpu_model"] = "Forged CPU"
    require_invalid(
        lambda: machine_metadata_from_host(drifted_cpu),
        "top-level CPU drift from M0 receipt was accepted",
    )

    drifted_ram = copy.deepcopy(host)
    drifted_ram["physical_ram_mib"] = 65536
    require_invalid(
        lambda: machine_metadata_from_host(drifted_ram),
        "top-level RAM drift from M0 receipt was accepted",
    )

    bad_timestamp = copy.deepcopy(host)
    bad_timestamp["benchmark_machine_metadata"]["captured_at_utc"] = "not-a-time"
    require_invalid(
        lambda: machine_metadata_from_host(bad_timestamp),
        "invalid M0 capture timestamp was accepted",
    )

    print("M7 physical host evidence authority tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
