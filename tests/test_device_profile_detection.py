from zevryon_platform.performance_contract import DeviceClass, select_device_class


def test_profile_thresholds() -> None:
    assert select_device_class(2048) is DeviceClass.LEGACY_PHONE
    assert select_device_class(4096) is DeviceClass.MID_PHONE
    assert select_device_class(8192) is DeviceClass.MODERN_PHONE
    assert select_device_class(16384) is DeviceClass.DESKTOP
