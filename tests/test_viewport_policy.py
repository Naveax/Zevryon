from zevryon_platform.memory_controller import decide_memory_pressure
from zevryon_platform.performance_contract import DeviceClass
from zevryon_platform.viewport_policy import viewport_policy


def test_viewport_materialization_shrinks_under_pressure() -> None:
    normal = viewport_policy(decide_memory_pressure(DeviceClass.LEGACY_PHONE, 20.0))
    soft = viewport_policy(decide_memory_pressure(DeviceClass.LEGACY_PHONE, 50.0))
    hard = viewport_policy(decide_memory_pressure(DeviceClass.LEGACY_PHONE, 60.0))
    survival = viewport_policy(decide_memory_pressure(DeviceClass.LEGACY_PHONE, 80.0))

    assert normal.max_materialized_records > soft.max_materialized_records
    assert soft.max_materialized_records > hard.max_materialized_records
    assert hard.max_materialized_records > survival.max_materialized_records
    assert survival.warm_record_limit == 0
    assert survival.max_record_preview_bytes == 4096
