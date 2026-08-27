#!/usr/bin/env python3
from __future__ import annotations

from typing import Mapping

from browser_competitor_benchmark_evidence import normalized_system_fingerprint
from zevryon_platform.benchmark_metadata import (
    BenchmarkMachineMetadata,
    ThermalObservation,
    ThermalState,
    physical_certification_checks,
)
from zevryon_platform.performance_contract import DeviceClass


PHYSICAL_HOST_AUTHORITY = "m0-benchmark-machine-metadata-physical-certification-v1"


class PhysicalHostEvidenceInvalid(ValueError):
    pass


def _mapping(value: object, field: str) -> Mapping[str, object]:
    if not isinstance(value, Mapping):
        raise PhysicalHostEvidenceInvalid(f"{field} must be an object")
    return value


def _required_text(value: object, field: str) -> str:
    if not isinstance(value, str) or not value.strip():
        raise PhysicalHostEvidenceInvalid(f"{field} must be non-empty text")
    return value.strip()


def machine_metadata_from_host(host: Mapping[str, object]) -> BenchmarkMachineMetadata:
    raw = _mapping(
        host.get("benchmark_machine_metadata"),
        "benchmark_machine_metadata",
    )
    thermal_raw = _mapping(raw.get("thermal"), "benchmark_machine_metadata.thermal")
    readings_raw = thermal_raw.get("readings_c")
    if not isinstance(readings_raw, list):
        raise PhysicalHostEvidenceInvalid(
            "benchmark_machine_metadata.thermal.readings_c must be an array"
        )
    try:
        readings = tuple(float(value) for value in readings_raw)
        metadata = BenchmarkMachineMetadata(
            schema_version=int(raw.get("schema_version", 0)),
            captured_at_utc=_required_text(
                raw.get("captured_at_utc"),
                "benchmark_machine_metadata.captured_at_utc",
            ),
            device_class=DeviceClass(
                _required_text(
                    raw.get("device_class"),
                    "benchmark_machine_metadata.device_class",
                )
            ),
            physical_device_confirmed=raw.get("physical_device_confirmed") is True,
            physical_ram_mib=int(raw.get("physical_ram_mib", 0)),
            logical_cpu_count=int(raw.get("logical_cpu_count", 0)),
            os_name=_required_text(raw.get("os_name"), "benchmark_machine_metadata.os_name"),
            os_release=_required_text(
                raw.get("os_release"),
                "benchmark_machine_metadata.os_release",
            ),
            architecture=_required_text(
                raw.get("architecture"),
                "benchmark_machine_metadata.architecture",
            ),
            cpu_model=_required_text(
                raw.get("cpu_model"),
                "benchmark_machine_metadata.cpu_model",
            ),
            run_label=_required_text(
                raw.get("run_label"),
                "benchmark_machine_metadata.run_label",
            ),
            thermal=ThermalObservation(
                state=ThermalState(
                    _required_text(
                        thermal_raw.get("state"),
                        "benchmark_machine_metadata.thermal.state",
                    )
                ),
                source=_required_text(
                    thermal_raw.get("source"),
                    "benchmark_machine_metadata.thermal.source",
                ),
                readings_c=readings,
            ),
        )
        metadata.validate()
    except (TypeError, ValueError) as exc:
        if isinstance(exc, PhysicalHostEvidenceInvalid):
            raise
        raise PhysicalHostEvidenceInvalid(
            f"M0 benchmark machine metadata is invalid: {exc}"
        ) from exc

    stable_expectations = {
        "machine_metadata_schema": metadata.schema_version,
        "platform": metadata.os_name,
        "arch": metadata.architecture,
        "kernel": metadata.os_release,
        "logical_cpus": metadata.logical_cpu_count,
        "cpu_model": metadata.cpu_model,
        "physical_ram_mib": metadata.physical_ram_mib,
        "device_class": metadata.device_class.value,
    }
    drift = [
        field
        for field, expected in stable_expectations.items()
        if host.get(field) != expected
    ]
    if drift:
        raise PhysicalHostEvidenceInvalid(
            "top-level stable host identity drifted from M0 receipt: "
            + ", ".join(drift)
        )

    try:
        normalized_system_fingerprint(host)
    except (TypeError, ValueError) as exc:
        raise PhysicalHostEvidenceInvalid(
            f"stable system fingerprint authority rejected host receipt: {exc}"
        ) from exc
    return metadata


def certify_physical_host(
    host: Mapping[str, object],
    *,
    label: str,
) -> dict[str, object]:
    metadata = machine_metadata_from_host(host)
    checks = physical_certification_checks(metadata)
    if checks.get("physical_metadata_complete") is not True:
        missing = sorted(key for key, passed in checks.items() if not passed)
        raise PhysicalHostEvidenceInvalid(
            f"{label} lacks canonical physical benchmark evidence: "
            + ", ".join(missing)
        )
    return {
        "authority": PHYSICAL_HOST_AUTHORITY,
        "label": label,
        "captured_at_utc": metadata.captured_at_utc,
        "device_class": metadata.device_class.value,
        "physical_ram_mib": metadata.physical_ram_mib,
        "logical_cpu_count": metadata.logical_cpu_count,
        "cpu_model": metadata.cpu_model,
        "thermal": {
            "state": metadata.thermal.state.value,
            "source": metadata.thermal.source,
            "readings_c": list(metadata.thermal.readings_c),
        },
        "checks": checks,
        "physical_host_gate_passed": True,
    }
