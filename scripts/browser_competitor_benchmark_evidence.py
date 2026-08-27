#!/usr/bin/env python3
from __future__ import annotations

from dataclasses import asdict, dataclass
import hashlib
import json
import os
import platform
import re
from typing import Mapping

from browser_competitor_scenario_contract import scenario_semantics
from m7_synthetic_corpus import (
    CORPUS_CHUNK_BYTES,
    SYNTHETIC_PATTERN,
    synthetic_corpus_sha256,
)


HARNESS_SCHEMA = "zevryon.competitor.giant-document.v2"
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
    return {
        "platform": platform.system() or "unknown",
        "arch": platform.machine() or "unknown",
        "kernel": platform.release() or "unknown",
        "logical_cpus": int(os.cpu_count() or 0),
    }


def normalized_system_fingerprint(host: Mapping[str, object]) -> str:
    normalized = {
        "platform": str(host.get("platform", "")).strip(),
        "arch": str(host.get("arch", "")).strip(),
        "kernel": str(host.get("kernel", "")).strip(),
        "logical_cpus": int(host.get("logical_cpus", 0)),
    }
    if not normalized["platform"] or not normalized["arch"] or not normalized["kernel"]:
        raise ValueError("system fingerprint requires platform, arch, and kernel")
    if normalized["logical_cpus"] < 0:
        raise ValueError("system fingerprint logical_cpus cannot be negative")
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
