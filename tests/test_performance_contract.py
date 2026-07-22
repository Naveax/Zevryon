from zevryon_platform.performance_contract import (
    BenchmarkObservation,
    DEVICE_PROFILES,
    DeviceClass,
    GIB,
    TITAN_WORST_CASE,
    evaluate,
)


def observation_for(device: DeviceClass) -> BenchmarkObservation:
    p = DEVICE_PROFILES[device]
    e = TITAN_WORST_CASE
    return BenchmarkObservation(
        device_class=device,
        logical_utf8_bytes=e.logical_utf8_bytes,
        logical_records=e.logical_records,
        logical_nodes=e.logical_nodes,
        style_runs=e.style_runs,
        resource_references=e.resource_references,
        process_group_pss_mb=p.process_group_pss_target_mb,
        first_viewport_preindexed_ms=p.first_viewport_preindexed_ms,
        first_viewport_streaming_ms=p.first_viewport_streaming_ms,
        scroll_p99_ms=p.scroll_p99_ms,
        maximum_normal_stall_ms=p.maximum_normal_stall_ms,
        exact_search_warm_ms=p.exact_search_warm_ms,
        exact_search_cold_ms=p.exact_search_cold_ms,
        mutation_p95_us=p.mutation_p95_us,
        copy_throughput_mib_s=p.copy_throughput_mib_s,
    )


def test_contract_uses_bytes_not_message_average() -> None:
    assert TITAN_WORST_CASE.logical_utf8_bytes == 4 * GIB
    assert TITAN_WORST_CASE.logical_records != 1_000_000


def test_every_device_profile_can_score_100_at_its_target() -> None:
    for device in DeviceClass:
        checks = evaluate(observation_for(device))
        assert checks["score_100"], (device, checks)


def test_one_failed_gate_prevents_100() -> None:
    observation = observation_for(DeviceClass.LEGACY_PHONE)
    failed = BenchmarkObservation(**{
        **observation.__dict__,
        "process_group_pss_mb": observation.process_group_pss_mb + 0.01,
    })
    checks = evaluate(failed)
    assert not checks["memory_target"]
    assert checks["memory_hard_cap"]
    assert not checks["score_100"]


def test_all_profiles_use_decimal_mb_and_stay_below_160_mb() -> None:
    assert DEVICE_PROFILES[DeviceClass.LEGACY_PHONE].process_group_pss_target_mb == 64
    assert DEVICE_PROFILES[DeviceClass.DESKTOP].process_group_pss_hard_cap_mb == 160
    assert all(p.process_group_pss_hard_cap_mb <= 160 for p in DEVICE_PROFILES.values())


def test_crash_or_data_loss_can_never_be_compensated() -> None:
    observation = observation_for(DeviceClass.DESKTOP)
    failed = BenchmarkObservation(**{
        **observation.__dict__,
        "crashes_or_ooms": 1,
        "data_loss_events": 1,
    })
    checks = evaluate(failed)
    assert not checks["no_crash_or_oom"]
    assert not checks["zero_data_loss"]
    assert not checks["score_100"]
