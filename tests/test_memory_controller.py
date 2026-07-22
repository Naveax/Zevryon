from zevryon_platform.memory_controller import PressureLevel, decide_memory_pressure
from zevryon_platform.performance_contract import DEVICE_PROFILES, DeviceClass


def test_pressure_transitions_and_survival_actions() -> None:
    profile = DEVICE_PROFILES[DeviceClass.LEGACY_PHONE]
    assert decide_memory_pressure(DeviceClass.LEGACY_PHONE, 20).level is PressureLevel.NORMAL
    assert decide_memory_pressure(DeviceClass.LEGACY_PHONE, 50).level is PressureLevel.SOFT
    assert decide_memory_pressure(DeviceClass.LEGACY_PHONE, 60).level is PressureLevel.HARD
    survival = decide_memory_pressure(DeviceClass.LEGACY_PHONE, profile.process_group_pss_hard_cap_mb)
    assert survival.level is PressureLevel.SURVIVAL
    assert not survival.decode_images
    assert not survival.run_background_tasks
    assert survival.warm_cache_scale == 0.0
    assert survival.cold_cache_scale == 0.0


def test_desktop_does_not_spend_extra_ram_without_pressure_need() -> None:
    decision = decide_memory_pressure(DeviceClass.DESKTOP, 64)
    assert decision.target_mb == 128
    assert decision.hard_cap_mb == 160
    assert decision.level is PressureLevel.NORMAL
