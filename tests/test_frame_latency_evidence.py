from datetime import datetime, timezone

from zevryon_platform.benchmark_metadata import (
    BenchmarkMachineMetadata,
    ThermalObservation,
    ThermalState,
)
from zevryon_platform.frame_latency_evidence import (
    MIN_CERTIFICATION_SAMPLES,
    evaluate_frame_latencies,
    frame_sample_sha256,
    nearest_rank_percentile,
    parse_frame_samples,
)
from zevryon_platform.performance_contract import DeviceClass


def machine(
    *,
    thermal_state: ThermalState = ThermalState.NOMINAL,
    physical: bool = True,
) -> BenchmarkMachineMetadata:
    return BenchmarkMachineMetadata(
        schema_version=1,
        captured_at_utc=datetime(2026, 8, 21, 12, 0, tzinfo=timezone.utc)
        .isoformat()
        .replace("+00:00", "Z"),
        device_class=DeviceClass.DESKTOP,
        physical_device_confirmed=physical,
        physical_ram_mib=16_384,
        logical_cpu_count=16,
        os_name="Linux",
        os_release="test",
        architecture="x86_64",
        cpu_model="test-cpu",
        run_label="frame-certification-test",
        thermal=ThermalObservation(thermal_state, "test", (50.0,)),
    )


def test_nearest_rank_is_conservative_and_deterministic() -> None:
    values = [float(value) for value in range(1, 1001)]
    assert nearest_rank_percentile(values, 50.0) == 500.0
    assert nearest_rank_percentile(values, 95.0) == 950.0
    assert nearest_rank_percentile(values, 99.0) == 990.0
    assert frame_sample_sha256(values) == frame_sample_sha256(tuple(values))


def test_desktop_profile_can_certify_at_measured_target() -> None:
    evidence = evaluate_frame_latencies([8.0] * MIN_CERTIFICATION_SAMPLES, machine())
    assert evidence.certified
    assert evidence.summary.p99_ms == 8.0
    assert evidence.p99_threshold_ms == 8.33
    assert evidence.summary.frames_over_50ms == 0
    assert evidence.summary.frames_over_75ms == 0


def test_p99_regression_fails_profile_gate() -> None:
    samples = [8.0] * 989 + [9.0] * 11
    evidence = evaluate_frame_latencies(samples, machine())
    assert evidence.summary.p99_ms == 9.0
    assert not evidence.checks["p99_within_profile"]
    assert not evidence.certified


def test_sample_count_physical_and_thermal_gates_fail_closed() -> None:
    assert not evaluate_frame_latencies(
        [8.0] * (MIN_CERTIFICATION_SAMPLES - 1), machine()
    ).certified
    assert not evaluate_frame_latencies(
        [8.0] * MIN_CERTIFICATION_SAMPLES, machine(physical=False)
    ).certified
    serious = evaluate_frame_latencies(
        [8.0] * MIN_CERTIFICATION_SAMPLES,
        machine(thermal_state=ThermalState.SERIOUS),
    )
    assert not serious.checks["thermal_state_stable"]
    assert not serious.certified


def test_parser_accepts_json_and_text_but_rejects_invalid_samples() -> None:
    assert parse_frame_samples("[1,2,3]") == (1.0, 2.0, 3.0)
    assert parse_frame_samples("1\n2, 3") == (1.0, 2.0, 3.0)
    try:
        parse_frame_samples("1 -2 3")
    except ValueError:
        pass
    else:
        raise AssertionError("negative frame latency was accepted")
