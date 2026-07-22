from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from pathlib import Path
from typing import Iterable

from .performance_contract import DEVICE_PROFILES, DeviceClass


class PressureLevel(str, Enum):
    NORMAL = "normal"
    SOFT = "soft-pressure"
    HARD = "hard-pressure"
    SURVIVAL = "survival"


@dataclass(frozen=True)
class MemoryDecision:
    level: PressureLevel
    measured_pss_mb: float
    target_mb: int
    hard_cap_mb: int
    hot_cache_scale: float
    warm_cache_scale: float
    cold_cache_scale: float
    overscan_scale: float
    decode_images: bool
    run_background_tasks: bool
    materialize_accessibility_window_only: bool


def decide_memory_pressure(device_class: DeviceClass, measured_pss_mb: float) -> MemoryDecision:
    profile = DEVICE_PROFILES[device_class]
    ratio = measured_pss_mb / profile.process_group_pss_target_mb
    if measured_pss_mb >= profile.process_group_pss_hard_cap_mb:
        level = PressureLevel.SURVIVAL
        scales = (0.15, 0.0, 0.0, 0.25)
        decode_images = False
        background = False
        a11y_window = True
    elif ratio >= 0.92:
        level = PressureLevel.HARD
        scales = (0.35, 0.1, 0.0, 0.5)
        decode_images = False
        background = False
        a11y_window = True
    elif ratio >= 0.75:
        level = PressureLevel.SOFT
        scales = (0.7, 0.4, 0.25, 0.75)
        decode_images = True
        background = False
        a11y_window = False
    else:
        level = PressureLevel.NORMAL
        scales = (1.0, 1.0, 1.0, 1.0)
        decode_images = True
        background = True
        a11y_window = False
    return MemoryDecision(
        level=level,
        measured_pss_mb=measured_pss_mb,
        target_mb=profile.process_group_pss_target_mb,
        hard_cap_mb=profile.process_group_pss_hard_cap_mb,
        hot_cache_scale=scales[0],
        warm_cache_scale=scales[1],
        cold_cache_scale=scales[2],
        overscan_scale=scales[3],
        decode_images=decode_images,
        run_background_tasks=background,
        materialize_accessibility_window_only=a11y_window,
    )


def linux_process_group_pss_mb(pids: Iterable[int]) -> float:
    """Read proportional set size without counting shared pages once per process as RSS does."""
    total_kib = 0
    for pid in set(pids):
        path = Path(f"/proc/{pid}/smaps_rollup")
        try:
            for line in path.read_text(encoding="ascii", errors="ignore").splitlines():
                if line.startswith("Pss:"):
                    total_kib += int(line.split()[1])
                    break
        except (FileNotFoundError, PermissionError, ProcessLookupError):
            continue
    return total_kib * 1024 / 1_000_000
