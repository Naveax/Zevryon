#!/usr/bin/env python3
from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
import math
import os
import platform
import re
from typing import Mapping


_SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
_UNICODE_PATTERN = "👨‍👩‍👧‍👦👍🏽🚀 ".encode("utf-8")
_DEFAULT_CHUNK_BYTES = 1024 * 1024
_BENCHMARK_MODES = frozenset({"virtualized", "native-dom"})
_QUERY_SEQUENCE = "lcg32-seed-243f6a88-mul-1664525-add-1013904223-v1"
_UNICODE_GENERATOR = "zevryon-emoji-chunk-doubling-v1"


def _require_text(value: object, field: str) -> str:
    if not isinstance(value, str) or not value.strip():
        raise ValueError(f"{field} must be non-empty text")
    return value.strip()


def _validate_json_numbers(value: object) -> None:
    if isinstance(value, float) and not math.isfinite(value):
        raise ValueError("canonical evidence JSON forbids NaN and infinity")
    if isinstance(value, Mapping):
        for key, child in value.items():
            if not isinstance(key, str):
                raise ValueError("canonical evidence JSON requires string object keys")
            _validate_json_numbers(child)
    elif isinstance(value, (list, tuple)):
        for child in value:
            _validate_json_numbers(child)


def canonical_json_bytes(value: object) -> bytes:
    _validate_json_numbers(value)
    try:
        text = json.dumps(
            value,
            ensure_ascii=False,
            sort_keys=True,
            separators=(",", ":"),
            allow_nan=False,
        )
    except (TypeError, ValueError) as exc:
        raise ValueError(f"value is not canonical-evidence JSON: {exc}") from exc
    return text.encode("utf-8")


def canonical_sha256(value: object) -> str:
    return hashlib.sha256(canonical_json_bytes(value)).hexdigest()


def capture_host_state() -> tuple[str, str, dict[str, object]]:
    host_platform = _require_text(platform.system(), "host_platform")
    host_arch = _require_text(platform.machine(), "host_arch")
    state: dict[str, object] = {
        "platform": host_platform,
        "arch": host_arch,
        "platform_release": _require_text(platform.release(), "platform_release"),
        "python_implementation": _require_text(
            platform.python_implementation(), "python_implementation"
        ),
        "python_version": _require_text(platform.python_version(), "python_version"),
        "logical_cpus": os.cpu_count(),
    }
    return host_platform, host_arch, state


def _payload_chunk(chunk_bytes: int = _DEFAULT_CHUNK_BYTES) -> bytes:
    if chunk_bytes <= 0:
        raise ValueError("chunk_bytes must be positive")
    chunk = bytearray(chunk_bytes)
    seeded = min(len(_UNICODE_PATTERN), chunk_bytes)
    chunk[:seeded] = _UNICODE_PATTERN[:seeded]
    filled = seeded
    while filled < chunk_bytes:
        copy = min(filled, chunk_bytes - filled)
        if copy <= 0:
            raise ValueError("Unicode payload pattern cannot be empty")
        chunk[filled : filled + copy] = chunk[:copy]
        filled += copy
    return bytes(chunk)


def unicode_payload_sha256(
    payload_bytes: int,
    *,
    chunk_bytes: int = _DEFAULT_CHUNK_BYTES,
) -> str:
    if payload_bytes <= 0:
        raise ValueError("payload_bytes must be positive")
    chunk = _payload_chunk(chunk_bytes)
    full_chunks, remainder = divmod(payload_bytes, len(chunk))
    digest = hashlib.sha256()
    for _ in range(full_chunks):
        digest.update(chunk)
    if remainder:
        digest.update(chunk[:remainder])
    return digest.hexdigest()


def benchmark_scenario(
    *,
    mode: str,
    payload_bytes: int,
    query_count: int,
    virtual_slice_bytes: int,
    timeout_seconds: int,
    viewport_width: int = 800,
    viewport_height: int = 720,
    warmup_policy: str = "gc-if-exposed-then-250ms-v1",
    memory_accounting_definition: str,
    harness_schema: str,
) -> dict[str, object]:
    if mode not in _BENCHMARK_MODES:
        raise ValueError(f"unsupported benchmark mode: {mode}")
    for field, value in (
        ("payload_bytes", payload_bytes),
        ("query_count", query_count),
        ("virtual_slice_bytes", virtual_slice_bytes),
        ("timeout_seconds", timeout_seconds),
        ("viewport_width", viewport_width),
        ("viewport_height", viewport_height),
    ):
        if not isinstance(value, int) or value <= 0:
            raise ValueError(f"{field} must be a positive integer")
    return {
        "mode": mode,
        "payload_bytes": payload_bytes,
        "query_count": query_count,
        "virtual_slice_bytes": virtual_slice_bytes,
        "timeout_seconds": timeout_seconds,
        "viewport": {"width": viewport_width, "height": viewport_height},
        "unicode_payload_generator": _UNICODE_GENERATOR,
        "query_sequence": _QUERY_SEQUENCE,
        "warmup_policy": _require_text(warmup_policy, "warmup_policy"),
        "memory_accounting_definition": _require_text(
            memory_accounting_definition, "memory_accounting_definition"
        ),
        "harness_schema": _require_text(harness_schema, "harness_schema"),
    }


def system_state_fingerprint(state: Mapping[str, object]) -> str:
    if not isinstance(state, Mapping) or not state:
        raise ValueError("system state must be a non-empty mapping")
    return canonical_sha256(dict(state))


def scenario_fingerprint(scenario: Mapping[str, object]) -> str:
    if not isinstance(scenario, Mapping) or not scenario:
        raise ValueError("scenario must be a non-empty mapping")
    return canonical_sha256(dict(scenario))


def _require_sha256(value: str, field: str) -> str:
    if _SHA256_RE.fullmatch(value) is None:
        raise ValueError(f"{field} must be lowercase SHA-256")
    return value


@dataclass(frozen=True)
class EvidenceContext:
    host_platform: str
    host_arch: str
    system_fingerprint: str
    harness_schema: str
    corpus_sha256: str
    scenario_fingerprint: str

    def validate(self) -> None:
        _require_text(self.host_platform, "host_platform")
        _require_text(self.host_arch, "host_arch")
        _require_text(self.harness_schema, "harness_schema")
        _require_sha256(self.system_fingerprint, "system_fingerprint")
        _require_sha256(self.corpus_sha256, "corpus_sha256")
        _require_sha256(self.scenario_fingerprint, "scenario_fingerprint")

    def terminal_kwargs(self) -> dict[str, str]:
        self.validate()
        return {
            "host_platform": self.host_platform,
            "host_arch": self.host_arch,
            "system_fingerprint": self.system_fingerprint,
            "harness_schema": self.harness_schema,
            "corpus_sha256": self.corpus_sha256,
            "scenario_fingerprint": self.scenario_fingerprint,
        }


def build_evidence_context(
    *,
    host_platform: str,
    host_arch: str,
    system_state: Mapping[str, object],
    harness_schema: str,
    payload_bytes: int,
    scenario: Mapping[str, object],
) -> EvidenceContext:
    platform_text = _require_text(host_platform, "host_platform")
    arch_text = _require_text(host_arch, "host_arch")
    schema = _require_text(harness_schema, "harness_schema")
    if payload_bytes <= 0:
        raise ValueError("payload_bytes must be positive")
    if scenario.get("harness_schema") != schema:
        raise ValueError("scenario harness schema does not match evidence context")
    if scenario.get("payload_bytes") != payload_bytes:
        raise ValueError("scenario payload size does not match corpus evidence")

    normalized_state = dict(system_state)
    if normalized_state.get("platform") != platform_text:
        raise ValueError("system-state platform does not match evidence host platform")
    if normalized_state.get("arch") != arch_text:
        raise ValueError("system-state architecture does not match evidence host architecture")

    context = EvidenceContext(
        host_platform=platform_text,
        host_arch=arch_text,
        system_fingerprint=system_state_fingerprint(normalized_state),
        harness_schema=schema,
        corpus_sha256=unicode_payload_sha256(payload_bytes),
        scenario_fingerprint=scenario_fingerprint(scenario),
    )
    context.validate()
    return context
