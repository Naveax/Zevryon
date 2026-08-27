from datetime import datetime, timezone
import json

import pytest

from zevryon_platform.benchmark_metadata import (
    ThermalState,
    capture_benchmark_metadata,
    physical_certification_checks,
)
from zevryon_platform.performance_contract import DeviceClass


def test_physical_evidence_requires_explicit_confirmation_and_thermal() -> None:
    meta = capture_benchmark_metadata(
        env={
            "ZEVRYON_PHYSICAL_DEVICE": "1",
            "ZEVRYON_THERMAL_STATE": "nominal",
            "ZEVRYON_THERMAL_C": "51.5,52.0",
            "ZEVRYON_CPU_MODEL": "Test CPU",
            "ZEVRYON_BENCHMARK_RUN_LABEL": "lab-a",
        },
        captured_at=datetime(2026, 8, 21, 12, 0, tzinfo=timezone.utc),
        physical_ram_mib_override=16384,
        logical_cpu_count_override=16,
    )
    checks = physical_certification_checks(meta)
    assert checks["physical_metadata_complete"]
    assert meta.device_class is DeviceClass.DESKTOP
    assert meta.thermal.state is ThermalState.NOMINAL
    assert meta.thermal.readings_c == (51.5, 52.0)


def test_unconfirmed_machine_cannot_be_physical_certification() -> None:
    meta = capture_benchmark_metadata(
        env={"ZEVRYON_CPU_MODEL": "CI CPU"},
        captured_at=datetime(2026, 8, 21, 12, 0, tzinfo=timezone.utc),
        physical_ram_mib_override=4096,
        logical_cpu_count_override=4,
    )
    checks = physical_certification_checks(meta)
    assert not checks["physical_device_confirmed"]
    assert not checks["physical_metadata_complete"]
    assert meta.device_class is DeviceClass.MID_PHONE


def test_json_is_deterministic_and_contains_no_hostname_or_username_fields() -> None:
    meta = capture_benchmark_metadata(
        env={
            "ZEVRYON_PHYSICAL_DEVICE": "1",
            "ZEVRYON_THERMAL_C": "40",
            "ZEVRYON_CPU_MODEL": "Deterministic CPU",
        },
        captured_at=datetime(2026, 8, 21, 12, 0, tzinfo=timezone.utc),
        physical_ram_mib_override=8192,
        logical_cpu_count_override=8,
    )
    encoded = meta.to_json()
    assert encoded == meta.to_json()
    payload = json.loads(encoded)
    assert "hostname" not in payload
    assert "username" not in payload
    assert payload["device_class"] == "modern-phone"


def test_windows_processor_identifier_is_preferred_over_generic_fallback() -> None:
    meta = capture_benchmark_metadata(
        env={
            "PROCESSOR_IDENTIFIER": "AMD64 Family 25 Model 33 Stepping 2, AuthenticAMD",
        },
        captured_at=datetime(2026, 8, 21, 12, 0, tzinfo=timezone.utc),
        physical_ram_mib_override=32768,
        logical_cpu_count_override=16,
    )
    # On Linux /proc/cpuinfo is a stronger authority and legitimately wins. On
    # platforms without /proc/cpuinfo (notably Windows), PROCESSOR_IDENTIFIER is
    # preferred before platform.processor() so the machine identity does not
    # collapse to a generic architecture label.
    if not __import__("pathlib").Path("/proc/cpuinfo").exists():
        assert meta.cpu_model == "AMD64 Family 25 Model 33 Stepping 2, AuthenticAMD"


def test_explicit_cpu_model_remains_highest_authority() -> None:
    meta = capture_benchmark_metadata(
        env={
            "ZEVRYON_CPU_MODEL": "Explicit Benchmark CPU",
            "PROCESSOR_IDENTIFIER": "generic environment identity",
        },
        captured_at=datetime(2026, 8, 21, 12, 0, tzinfo=timezone.utc),
        physical_ram_mib_override=32768,
        logical_cpu_count_override=16,
    )
    assert meta.cpu_model == "Explicit Benchmark CPU"


def test_invalid_thermal_override_fails_closed() -> None:
    with pytest.raises(ValueError):
        capture_benchmark_metadata(
            env={"ZEVRYON_THERMAL_STATE": "volcanic"},
            physical_ram_mib_override=8192,
            logical_cpu_count_override=8,
        )

    with pytest.raises(ValueError):
        capture_benchmark_metadata(
            env={"ZEVRYON_THERMAL_C": "not-a-number"},
            physical_ram_mib_override=8192,
            logical_cpu_count_override=8,
        )
