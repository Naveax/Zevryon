#!/usr/bin/env python3
from __future__ import annotations

from dataclasses import asdict, dataclass
import hashlib
import json
from pathlib import Path
import re
import sys
from typing import Mapping

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from zevryon_platform.benchmark_metadata import capture_benchmark_metadata

from browser_competitor_scenario_contract import scenario_semantics
from m7_synthetic_corpus import (
    CORPUS_CHUNK_BYTES,
    SYNTHETIC_PATTERN,
    synthetic_corpus_sha256,
)


HARNESS_SCHEMA = "zevryon.competitor.giant-document.v2"
SYSTEM_FINGERPRINT_SCHEMA = "zevryon.competitor.system-fingerprint.v2"
_SHA256_RE = re.compile(r"^[0-9a-f]{64}$")


@dataclass(frozen=True)
class EvidenceIdentity:
    host_platform: str
    host_arch: str
    system_fingerprint: str
    harness_schema: str
    corpus_sha256: str
    scenario_fingerprint: str

    def as_terminal_kwargs(self) -> dict[str, str]:
        return asdict(self)


def canonical_sha256(value: object) -> str:
    encoded = json.dumps(
        value,
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=False,
    ).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def host_metadata() -> dict[str, object]:
    machine = capture_benchmark_metadata()
    machine_receipt = machine.to_dict()
    return {
        "system_fingerprint_schema": SYSTEM_FINGERPRINT_SCHEMA,
        "machine_metadata_schema": machine.schema_version,
        "platform": machine.os_name,
        "arch": machine.architecture,
        "kernel": machine.os_release,
        "logical_cpus": machine.logical_cpu_count,
        "cpu_model": machine.cpu_model,
        "physical_ram_mib": machine.physical_ram_mib,
        "device_class": machine.device_class.value,
        # Preserve the full existing M0 machine/thermal receipt as raw benchmark
        # evidence. normalized_system_fingerprint intentionally hashes only the
        # stable subset above so timestamps and thermal readings do not turn the
        # same physical host into a different machine identity between stages.
        "benchmark_machine_metadata": machine_receipt,
    }


def normalized_system_fingerprint(host: Mapping[str, object]) -> str:
    schema = str(host.get("system_fingerprint_schema", "")).strip()
    if schema != SYSTEM_FINGERPRINT_SCHEMA:
        raise ValueError(
            f"system fingerprint requires schema {SYSTEM_FINGERPRINT_SCHEMA}"
        )

    try:
        machine_metadata_schema = int(host.get("machine_metadata_schema", 0))
        logical_cpus = int(host.get("logical_cpus", 0))
        physical_ram_mib = int(host.get("physical_ram_mib", 0))
    except (TypeError, ValueError) as exc:
        raise ValueError("system fingerprint numeric host metadata is invalid") from exc

    normalized = {
        "schema": SYSTEM_FINGERPRINT_SCHEMA,
        "machine_metadata_schema": machine_metadata_schema,
        "platform": str(host.get("platform", "")).strip(),
        "arch": str(host.get("arch", "")).strip(),
        "kernel": str(host.get("kernel", "")).strip(),
        "logical_cpus": logical_cpus,
        "cpu_model": " ".join(str(host.get("cpu_model", "")).split()).strip(),
        "physical_ram_mib": physical_ram_mib,
        "device_class": str(host.get("device_class", "")).strip(),
    }
    for field in ("platform", "arch", "kernel", "cpu_model", "device_class"):
        if not normalized[field]:
            raise ValueError(f"system fingerprint requires {field}")
    if normalized["machine_metadata_schema"] <= 0:
        raise ValueError("system fingerprint machine metadata schema must be positive")
    if normalized["logical_cpus"] <= 0:
        raise ValueError("system fingerprint logical_cpus must be positive")
    if normalized["physical_ram_mib"] < 256:
        raise ValueError("system fingerprint physical RAM is implausibly small")
    return canonical_sha256(normalized)


def scenario_fingerprint(
    *,
    mode: str,
    payload_bytes: int,
    query_count: int,
    virtual_slice_bytes: int,
    timeout_seconds: int,
    warmup_query_count: int = 0,
) -> str:
    if mode not in {"virtualized", "native-dom"}:
        raise ValueError(f"unknown benchmark mode: {mode}")
    if (
        payload_bytes <= 0
        or query_count <= 0
        or virtual_slice_bytes <= 0
        or timeout_seconds <= 0
    ):
        raise ValueError("scenario fingerprint arguments must be positive")
    if (
        isinstance(warmup_query_count, bool)
        or not isinstance(warmup_query_count, int)
        or warmup_query_count < 0
    ):
        raise ValueError("warmup_query_count must be a non-negative integer")
    payload = {
        "schema": HARNESS_SCHEMA,
        "mode": mode,
        "payload_bytes": payload_bytes,
        "query_count": query_count,
        "warmup_query_count": warmup_query_count,
        "virtual_slice_bytes": (
            virtual_slice_bytes if mode == "virtualized" else None
        ),
        **scenario_semantics(mode),
        "timeout_seconds": timeout_seconds,
    }
    return canonical_sha256(payload)


def evidence_identity(
    *,
    host: Mapping[str, object],
    corpus_sha256: str,
    scenario_sha256: str,
) -> EvidenceIdentity:
    if _SHA256_RE.fullmatch(corpus_sha256) is None:
        raise ValueError("corpus_sha256 must be a lowercase SHA-256")
    if _SHA256_RE.fullmatch(scenario_sha256) is None:
        raise ValueError("scenario_sha256 must be a lowercase SHA-256")
    system_sha256 = normalized_system_fingerprint(host)
    host_platform = str(host.get("platform", "")).strip()
    host_arch = str(host.get("arch", "")).strip()
    if not host_platform or not host_arch:
        raise ValueError("evidence identity requires host platform and architecture")
    return EvidenceIdentity(
        host_platform=host_platform,
        host_arch=host_arch,
        system_fingerprint=system_sha256,
        harness_schema=HARNESS_SCHEMA,
        corpus_sha256=corpus_sha256,
        scenario_fingerprint=scenario_sha256,
    )
