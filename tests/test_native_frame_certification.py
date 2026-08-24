from datetime import datetime, timezone

from zevryon_platform.benchmark_metadata import (
    BenchmarkMachineMetadata,
    ThermalObservation,
    ThermalState,
)
from zevryon_platform.frame_latency_evidence import evaluate_frame_latencies
from zevryon_platform.native_frame_certification import (
    NativeFrameProbeSummary,
    build_native_frame_certification,
)
from zevryon_platform.performance_contract import DeviceClass


def machine() -> BenchmarkMachineMetadata:
    return BenchmarkMachineMetadata(
        schema_version=1,
        captured_at_utc=datetime(2026, 8, 21, tzinfo=timezone.utc)
        .isoformat()
        .replace("+00:00", "Z"),
        device_class=DeviceClass.DESKTOP,
        physical_device_confirmed=True,
        physical_ram_mib=16384,
        logical_cpu_count=16,
        os_name="TestOS",
        os_release="1",
        architecture="x86_64",
        cpu_model="Test CPU",
        run_label="native-frame-test",
        thermal=ThermalObservation(ThermalState.NOMINAL, "test", (50.0,)),
    )


def probe(**overrides: object) -> NativeFrameProbeSummary:
    value: dict[str, object] = {
        "operation": "zenith-tab-runtime-frame-probe",
        "profile": "desktop",
        "frame_budget_us": 8330,
        "warmup_samples": 120,
        "recorded_samples": 1000,
        "visible_layouts": 1120,
        "frame_overruns": 0,
        "prefetch_accepts": 10,
        "pool_thread_starts": 2,
        "pool_ready_peak_bytes": 65536,
    }
    value.update(overrides)
    return NativeFrameProbeSummary.from_mapping(value)


def test_native_frame_certification_passes_only_full_chain() -> None:
    evidence = evaluate_frame_latencies([1.0] * 1000, machine())
    certification = build_native_frame_certification(
        probe(), evidence, requested_samples=1000, requested_warmup=120
    )
    assert certification.certified


def test_probe_profile_mismatch_fails_closed() -> None:
    evidence = evaluate_frame_latencies([1.0] * 1000, machine())
    certification = build_native_frame_certification(
        probe(profile="mid-phone"),
        evidence,
        requested_samples=1000,
        requested_warmup=120,
    )
    assert not certification.certified
    assert not certification.checks["probe_profile_matches_machine"]


def test_probe_summary_rejects_extra_or_negative_fields() -> None:
    value = probe().to_dict()
    value["surprise"] = 1
    try:
        NativeFrameProbeSummary.from_mapping(value)
    except ValueError:
        pass
    else:
        raise AssertionError("extra probe field accepted")

    value = probe().to_dict()
    value["pool_thread_starts"] = -1
    try:
        NativeFrameProbeSummary.from_mapping(value)
    except ValueError:
        pass
    else:
        raise AssertionError("negative probe field accepted")
