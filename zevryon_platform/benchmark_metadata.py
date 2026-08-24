from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime, timezone
from enum import Enum
import json
import os
from pathlib import Path
import platform
from typing import Mapping

from .performance_contract import DeviceClass, physical_memory_mib, select_device_class

SCHEMA_VERSION = 1


class ThermalState(str, Enum):
    UNKNOWN = "unknown"
    NOMINAL = "nominal"
    FAIR = "fair"
    SERIOUS = "serious"
    CRITICAL = "critical"


@dataclass(frozen=True)
class ThermalObservation:
    state: ThermalState
    source: str
    readings_c: tuple[float, ...] = ()

    def validate(self) -> None:
        if not self.source or len(self.source) > 128:
            raise ValueError("thermal source must be 1..128 characters")
        if len(self.readings_c) > 64:
            raise ValueError("too many thermal readings")
        for value in self.readings_c:
            if not (-100.0 <= value <= 250.0):
                raise ValueError("thermal reading outside evidence range")

    @property
    def observed(self) -> bool:
        return self.state is not ThermalState.UNKNOWN or bool(self.readings_c)


@dataclass(frozen=True)
class BenchmarkMachineMetadata:
    schema_version: int
    captured_at_utc: str
    device_class: DeviceClass
    physical_device_confirmed: bool
    physical_ram_mib: int
    logical_cpu_count: int
    os_name: str
    os_release: str
    architecture: str
    cpu_model: str
    run_label: str
    thermal: ThermalObservation

    def validate(self) -> None:
        if self.schema_version != SCHEMA_VERSION:
            raise ValueError("unsupported benchmark metadata schema")
        parsed = datetime.fromisoformat(self.captured_at_utc.replace("Z", "+00:00"))
        if parsed.tzinfo is None or parsed.utcoffset() != timezone.utc.utcoffset(parsed):
            raise ValueError("captured_at_utc must be UTC")
        if self.physical_ram_mib < 256:
            raise ValueError("physical RAM metadata is implausibly small")
        if self.logical_cpu_count < 1 or self.logical_cpu_count > 4096:
            raise ValueError("logical CPU count outside evidence range")
        for name, value, limit in (
            ("os_name", self.os_name, 128),
            ("os_release", self.os_release, 256),
            ("architecture", self.architecture, 128),
            ("cpu_model", self.cpu_model, 512),
            ("run_label", self.run_label, 128),
        ):
            if not value or len(value) > limit:
                raise ValueError(f"{name} must be 1..{limit} characters")
        self.thermal.validate()

    def to_dict(self) -> dict[str, object]:
        return {
            "schema_version": self.schema_version,
            "captured_at_utc": self.captured_at_utc,
            "device_class": self.device_class.value,
            "physical_device_confirmed": self.physical_device_confirmed,
            "physical_ram_mib": self.physical_ram_mib,
            "logical_cpu_count": self.logical_cpu_count,
            "os_name": self.os_name,
            "os_release": self.os_release,
            "architecture": self.architecture,
            "cpu_model": self.cpu_model,
            "run_label": self.run_label,
            "thermal": {
                "state": self.thermal.state.value,
                "source": self.thermal.source,
                "readings_c": list(self.thermal.readings_c),
            },
        }

    def to_json(self) -> str:
        self.validate()
        return json.dumps(self.to_dict(), sort_keys=True, separators=(",", ":"))


def _bounded_text(value: str, fallback: str, limit: int) -> str:
    cleaned = " ".join(value.split()).strip()
    if not cleaned:
        cleaned = fallback
    return cleaned[:limit]


def _cpu_model(env: Mapping[str, str]) -> str:
    forced = env.get("ZEVRYON_CPU_MODEL", "").strip()
    if forced:
        return _bounded_text(forced, "unknown", 512)
    cpuinfo = Path("/proc/cpuinfo")
    if cpuinfo.exists():
        try:
            for line in cpuinfo.read_text(encoding="utf-8", errors="ignore").splitlines():
                if line.lower().startswith("model name") and ":" in line:
                    return _bounded_text(line.split(":", 1)[1], "unknown", 512)
        except OSError:
            pass
    return _bounded_text(platform.processor(), platform.machine() or "unknown", 512)


def _parse_thermal_override(env: Mapping[str, str]) -> ThermalObservation | None:
    raw_state = env.get("ZEVRYON_THERMAL_STATE")
    raw_readings = env.get("ZEVRYON_THERMAL_C")
    if raw_state is None and raw_readings is None:
        return None
    try:
        state = ThermalState(raw_state or ThermalState.UNKNOWN.value)
    except ValueError as exc:
        raise ValueError("ZEVRYON_THERMAL_STATE is invalid") from exc
    readings: tuple[float, ...] = ()
    if raw_readings:
        try:
            readings = tuple(float(part.strip()) for part in raw_readings.split(",") if part.strip())
        except ValueError as exc:
            raise ValueError("ZEVRYON_THERMAL_C must contain numeric Celsius values") from exc
    observation = ThermalObservation(state, "environment-override", readings)
    observation.validate()
    return observation


def _linux_thermal_readings() -> ThermalObservation | None:
    root = Path("/sys/class/thermal")
    if not root.exists():
        return None
    values: list[float] = []
    try:
        paths = sorted(root.glob("thermal_zone*/temp"))[:64]
    except OSError:
        return None
    for path in paths:
        try:
            raw = path.read_text(encoding="ascii", errors="ignore").strip()
            value = float(raw)
            if abs(value) > 1000.0:
                value /= 1000.0
            if -100.0 <= value <= 250.0:
                values.append(value)
        except (OSError, ValueError):
            continue
    if not values:
        return None
    return ThermalObservation(ThermalState.UNKNOWN, "linux-sysfs", tuple(values))


def capture_thermal_observation(env: Mapping[str, str] | None = None) -> ThermalObservation:
    source_env = os.environ if env is None else env
    override = _parse_thermal_override(source_env)
    if override is not None:
        return override
    if platform.system() == "Linux":
        observed = _linux_thermal_readings()
        if observed is not None:
            return observed
    return ThermalObservation(ThermalState.UNKNOWN, "unavailable", ())


def capture_benchmark_metadata(
    *,
    env: Mapping[str, str] | None = None,
    captured_at: datetime | None = None,
    physical_ram_mib_override: int | None = None,
    logical_cpu_count_override: int | None = None,
) -> BenchmarkMachineMetadata:
    source_env = os.environ if env is None else env
    ram_mib = physical_memory_mib() if physical_ram_mib_override is None else physical_ram_mib_override
    cpus = os.cpu_count() if logical_cpu_count_override is None else logical_cpu_count_override
    now = datetime.now(timezone.utc) if captured_at is None else captured_at.astimezone(timezone.utc)
    metadata = BenchmarkMachineMetadata(
        schema_version=SCHEMA_VERSION,
        captured_at_utc=now.isoformat().replace("+00:00", "Z"),
        device_class=select_device_class(ram_mib),
        physical_device_confirmed=source_env.get("ZEVRYON_PHYSICAL_DEVICE") == "1",
        physical_ram_mib=ram_mib,
        logical_cpu_count=max(1, cpus or 1),
        os_name=_bounded_text(platform.system(), "unknown", 128),
        os_release=_bounded_text(platform.release(), "unknown", 256),
        architecture=_bounded_text(platform.machine(), "unknown", 128),
        cpu_model=_cpu_model(source_env),
        run_label=_bounded_text(source_env.get("ZEVRYON_BENCHMARK_RUN_LABEL", "unlabeled"), "unlabeled", 128),
        thermal=capture_thermal_observation(source_env),
    )
    metadata.validate()
    return metadata


def physical_certification_checks(metadata: BenchmarkMachineMetadata) -> dict[str, bool]:
    metadata.validate()
    checks = {
        "physical_device_confirmed": metadata.physical_device_confirmed,
        "thermal_observed": metadata.thermal.observed,
        "device_profile_matches_ram": metadata.device_class == select_device_class(metadata.physical_ram_mib),
        "timestamp_is_utc": metadata.captured_at_utc.endswith("Z") or "+00:00" in metadata.captured_at_utc,
    }
    checks["physical_metadata_complete"] = all(checks.values())
    return checks
