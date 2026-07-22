from __future__ import annotations

from dataclasses import dataclass

from .memory_controller import MemoryDecision, PressureLevel


@dataclass(frozen=True)
class ViewportPolicy:
    max_materialized_records: int
    hot_record_limit: int
    warm_record_limit: int
    overscan_viewports: float
    max_record_preview_bytes: int


def viewport_policy(decision: MemoryDecision) -> ViewportPolicy:
    """Convert process-group pressure into bounded viewport residency.

    Source data remains in MassiveDoc. Pressure only changes transient materialization.
    """
    if decision.level is PressureLevel.SURVIVAL:
        return ViewportPolicy(96, 48, 0, 0.25, 4 * 1024)
    if decision.level is PressureLevel.HARD:
        return ViewportPolicy(160, 80, 128, 0.5, 8 * 1024)
    if decision.level is PressureLevel.SOFT:
        return ViewportPolicy(320, 160, 512, 0.75, 16 * 1024)
    return ViewportPolicy(512, 256, 2048, 1.5, 32 * 1024)
