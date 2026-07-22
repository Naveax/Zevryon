from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from typing import Mapping

MB = 1_000_000
MIB = 1024 * 1024
GIB = 1024 * MIB


class DeviceClass(str, Enum):
    LEGACY_PHONE = "legacy-phone"
    MID_PHONE = "mid-phone"
    MODERN_PHONE = "modern-phone"
    DESKTOP = "desktop"


@dataclass(frozen=True)
class ContentEnvelope:
    logical_utf8_bytes: int
    logical_records: int
    logical_nodes: int
    style_runs: int
    resource_references: int
    largest_record_bytes: int
    largest_unbroken_token_bytes: int
    pathological_grapheme_bytes: int

    def validate(self) -> None:
        for name, value in self.__dict__.items():
            if value < 0:
                raise ValueError(f"{name} must be non-negative")
        if self.logical_utf8_bytes == 0:
            raise ValueError("logical_utf8_bytes must be positive")
        if self.largest_record_bytes > self.logical_utf8_bytes:
            raise ValueError("largest_record_bytes exceeds total logical payload")
        if self.largest_unbroken_token_bytes > self.largest_record_bytes:
            raise ValueError("largest_unbroken_token_bytes exceeds largest record")


@dataclass(frozen=True)
class DeviceProfile:
    name: DeviceClass
    minimum_physical_ram_mib: int
    process_group_pss_target_mb: int
    process_group_pss_hard_cap_mb: int
    hot_cache_mb: int
    warm_cache_mb: int
    cold_cache_mb: int
    first_viewport_preindexed_ms: int
    first_viewport_streaming_ms: int
    scroll_p99_ms: float
    maximum_normal_stall_ms: int
    exact_search_warm_ms: int
    exact_search_cold_ms: int
    mutation_p95_us: int
    copy_throughput_mib_s: int

    def validate(self) -> None:
        cache_total = self.hot_cache_mb + self.warm_cache_mb + self.cold_cache_mb
        if cache_total >= self.process_group_pss_target_mb:
            raise ValueError("cache budget must leave room for engine/process metadata")
        if self.process_group_pss_target_mb >= self.process_group_pss_hard_cap_mb:
            raise ValueError("target must be below hard cap")
        if self.process_group_pss_hard_cap_mb > 160:
            raise ValueError("no profile may exceed the 160 MB absolute ceiling")
        if self.scroll_p99_ms <= 0:
            raise ValueError("scroll_p99_ms must be positive")


TITAN_WORST_CASE = ContentEnvelope(
    logical_utf8_bytes=4 * GIB,
    logical_records=8_388_608,
    logical_nodes=67_108_864,
    style_runs=33_554_432,
    resource_references=1_048_576,
    largest_record_bytes=64 * MIB,
    largest_unbroken_token_bytes=16 * MIB,
    pathological_grapheme_bytes=64 * 1024,
)

SURVIVAL_LOGICAL_BYTES_64BIT = 64 * GIB
SURVIVAL_LOGICAL_BYTES_32BIT = 16 * GIB

DEVICE_PROFILES: Mapping[DeviceClass, DeviceProfile] = {
    DeviceClass.LEGACY_PHONE: DeviceProfile(
        DeviceClass.LEGACY_PHONE, 2048, 64, 80, 6, 6, 4,
        5000, 2000, 33.3, 250, 350, 1500, 2000, 60,
    ),
    DeviceClass.MID_PHONE: DeviceProfile(
        DeviceClass.MID_PHONE, 4096, 80, 96, 8, 8, 6,
        2500, 1000, 16.6, 100, 180, 800, 1000, 120,
    ),
    DeviceClass.MODERN_PHONE: DeviceProfile(
        DeviceClass.MODERN_PHONE, 8192, 96, 128, 12, 12, 8,
        1500, 600, 11.1, 50, 120, 500, 500, 200,
    ),
    DeviceClass.DESKTOP: DeviceProfile(
        DeviceClass.DESKTOP, 8192, 128, 160, 18, 18, 10,
        1000, 300, 8.33, 16, 100, 300, 250, 400,
    ),
}


@dataclass(frozen=True)
class BenchmarkObservation:
    device_class: DeviceClass
    logical_utf8_bytes: int
    logical_records: int
    logical_nodes: int
    style_runs: int
    resource_references: int
    process_group_pss_mb: float
    first_viewport_preindexed_ms: float
    first_viewport_streaming_ms: float
    scroll_p99_ms: float
    maximum_normal_stall_ms: float
    exact_search_warm_ms: float
    exact_search_cold_ms: float
    mutation_p95_us: float
    copy_throughput_mib_s: float
    data_loss_events: int = 0
    invalid_utf8_events: int = 0
    crashes_or_ooms: int = 0


def evaluate(observation: BenchmarkObservation) -> dict[str, bool]:
    profile = DEVICE_PROFILES[observation.device_class]
    checks = {
        "certified_payload": observation.logical_utf8_bytes >= TITAN_WORST_CASE.logical_utf8_bytes,
        "certified_records": observation.logical_records >= TITAN_WORST_CASE.logical_records,
        "certified_nodes": observation.logical_nodes >= TITAN_WORST_CASE.logical_nodes,
        "certified_style_runs": observation.style_runs >= TITAN_WORST_CASE.style_runs,
        "certified_resources": observation.resource_references >= TITAN_WORST_CASE.resource_references,
        "memory_target": observation.process_group_pss_mb <= profile.process_group_pss_target_mb,
        "memory_hard_cap": observation.process_group_pss_mb <= profile.process_group_pss_hard_cap_mb,
        "first_viewport_preindexed": observation.first_viewport_preindexed_ms <= profile.first_viewport_preindexed_ms,
        "first_viewport_streaming": observation.first_viewport_streaming_ms <= profile.first_viewport_streaming_ms,
        "scroll_p99": observation.scroll_p99_ms <= profile.scroll_p99_ms,
        "stall_cap": observation.maximum_normal_stall_ms <= profile.maximum_normal_stall_ms,
        "search_warm": observation.exact_search_warm_ms <= profile.exact_search_warm_ms,
        "search_cold": observation.exact_search_cold_ms <= profile.exact_search_cold_ms,
        "mutation_p95": observation.mutation_p95_us <= profile.mutation_p95_us,
        "copy_throughput": observation.copy_throughput_mib_s >= profile.copy_throughput_mib_s,
        "zero_data_loss": observation.data_loss_events == 0,
        "valid_utf8": observation.invalid_utf8_events == 0,
        "no_crash_or_oom": observation.crashes_or_ooms == 0,
    }
    checks["score_100"] = all(checks.values())
    return checks


for _profile in DEVICE_PROFILES.values():
    _profile.validate()
TITAN_WORST_CASE.validate()


def physical_memory_mib() -> int:
    import os
    import platform
    import subprocess

    override = os.environ.get("ZEVRYON_PHYSICAL_RAM_MIB")
    if override:
        try:
            return max(256, int(override))
        except ValueError as exc:
            raise ValueError("ZEVRYON_PHYSICAL_RAM_MIB must be an integer") from exc
    if os.name == "nt":
        import ctypes

        class MemoryStatusEx(ctypes.Structure):
            _fields_ = [
                ("dwLength", ctypes.c_ulong), ("dwMemoryLoad", ctypes.c_ulong),
                ("ullTotalPhys", ctypes.c_ulonglong), ("ullAvailPhys", ctypes.c_ulonglong),
                ("ullTotalPageFile", ctypes.c_ulonglong), ("ullAvailPageFile", ctypes.c_ulonglong),
                ("ullTotalVirtual", ctypes.c_ulonglong), ("ullAvailVirtual", ctypes.c_ulonglong),
                ("ullAvailExtendedVirtual", ctypes.c_ulonglong),
            ]

        status = MemoryStatusEx()
        status.dwLength = ctypes.sizeof(status)
        if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status)):
            return max(256, int(status.ullTotalPhys // MIB))
    meminfo = __import__("pathlib").Path("/proc/meminfo")
    if meminfo.exists():
        for line in meminfo.read_text(encoding="ascii", errors="ignore").splitlines():
            if line.startswith("MemTotal:"):
                return max(256, int(line.split()[1]) // 1024)
    if platform.system() == "Darwin":
        try:
            value = subprocess.check_output(["sysctl", "-n", "hw.memsize"], text=True, timeout=2)
            return max(256, int(value.strip()) // MIB)
        except Exception:
            pass
    return 4096


def select_device_class(total_ram_mib: int | None = None) -> DeviceClass:
    import os

    forced = os.environ.get("ZEVRYON_DEVICE_PROFILE")
    if forced:
        return DeviceClass(forced)
    ram = physical_memory_mib() if total_ram_mib is None else total_ram_mib
    if ram < 3072:
        return DeviceClass.LEGACY_PHONE
    if ram < 6144:
        return DeviceClass.MID_PHONE
    if ram < 12_288:
        return DeviceClass.MODERN_PHONE
    return DeviceClass.DESKTOP
