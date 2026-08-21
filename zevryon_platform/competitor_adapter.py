from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime, timezone
import json
import math
import subprocess
from typing import Mapping, Sequence

from zevryon_platform.competitor_lab import Engine, LabSystemState, system_state_from_mapping
from zevryon_platform.competitor_lab_v2 import (
    WorkloadBoundRun,
    bound_run_from_mapping,
    canonical_workload_sha256,
)

PROTOCOL = "zevryon.m7.adapter.v1"
WORKLOAD_SCHEMA = "zevryon.m7.workload.v1"


def _sha256(value: str, name: str) -> str:
    if len(value) != 64 or any(ch not in "0123456789abcdef" for ch in value):
        raise ValueError(f"{name} must be lowercase hexadecimal SHA-256")
    return value


def _text(value: object, name: str, maximum: int) -> str:
    if not isinstance(value, str):
        raise ValueError(f"{name} must be a string")
    cleaned = " ".join(value.split()).strip()
    if not cleaned or len(cleaned) > maximum:
        raise ValueError(f"{name} must be 1..{maximum} characters")
    return cleaned


def validate_workload(
    workload: Mapping[str, object],
    corpus_sha256: str,
    corpus_logical_bytes: int,
) -> None:
    required = {
        "schema",
        "corpus_sha256",
        "corpus_logical_bytes",
        "viewport",
        "operations",
    }
    if not required.issubset(workload):
        raise ValueError("workload is missing canonical required fields")
    if workload["schema"] != WORKLOAD_SCHEMA:
        raise ValueError("unsupported M7 workload schema")
    if workload["corpus_sha256"] != corpus_sha256:
        raise ValueError("workload corpus_sha256 does not match request")
    if workload["corpus_logical_bytes"] != corpus_logical_bytes:
        raise ValueError("workload corpus_logical_bytes does not match request")
    if not isinstance(workload["viewport"], Mapping):
        raise ValueError("workload viewport must be an object")
    operations = workload["operations"]
    if not isinstance(operations, list) or not operations:
        raise ValueError("workload operations must be a non-empty array")
    if any(not isinstance(operation, Mapping) for operation in operations):
        raise ValueError("every workload operation must be an object")


@dataclass(frozen=True)
class AdapterRequest:
    engine: Engine
    run_index: int
    workload_sha256: str
    corpus_sha256: str
    corpus_logical_bytes: int
    corpus_path: str
    workload: Mapping[str, object]
    system_state: LabSystemState

    def validate(self) -> None:
        if self.run_index < 0:
            raise ValueError("run_index must be non-negative")
        _sha256(self.workload_sha256, "workload_sha256")
        _sha256(self.corpus_sha256, "corpus_sha256")
        if self.corpus_logical_bytes <= 0:
            raise ValueError("corpus_logical_bytes must be positive")
        _text(self.corpus_path, "corpus_path", 4096)
        self.system_state.validate()
        validate_workload(self.workload, self.corpus_sha256, self.corpus_logical_bytes)
        if canonical_workload_sha256(self.workload) != self.workload_sha256:
            raise ValueError("workload_sha256 does not match canonical workload JSON")

    def to_dict(self) -> dict[str, object]:
        self.validate()
        return {
            "protocol": PROTOCOL,
            "engine": self.engine.value,
            "run_index": self.run_index,
            "workload_sha256": self.workload_sha256,
            "corpus_sha256": self.corpus_sha256,
            "corpus_logical_bytes": self.corpus_logical_bytes,
            "corpus_path": self.corpus_path,
            "workload": dict(self.workload),
            "system_state": self.system_state.to_dict(),
        }


def adapter_request_from_mapping(value: Mapping[str, object]) -> AdapterRequest:
    required = {
        "protocol",
        "engine",
        "run_index",
        "workload_sha256",
        "corpus_sha256",
        "corpus_logical_bytes",
        "corpus_path",
        "workload",
        "system_state",
    }
    if set(value) != required:
        raise ValueError("adapter request fields mismatch")
    if value["protocol"] != PROTOCOL:
        raise ValueError("unsupported M7 adapter protocol")
    if not isinstance(value["run_index"], int) or isinstance(value["run_index"], bool):
        raise ValueError("run_index must be an integer")
    logical = value["corpus_logical_bytes"]
    if not isinstance(logical, int) or isinstance(logical, bool):
        raise ValueError("corpus_logical_bytes must be an integer")
    workload = value["workload"]
    state = value["system_state"]
    if not isinstance(workload, Mapping) or not isinstance(state, Mapping):
        raise ValueError("workload and system_state must be objects")
    request = AdapterRequest(
        engine=Engine(str(value["engine"])),
        run_index=value["run_index"],
        workload_sha256=str(value["workload_sha256"]),
        corpus_sha256=str(value["corpus_sha256"]),
        corpus_logical_bytes=logical,
        corpus_path=str(value["corpus_path"]),
        workload=workload,
        system_state=system_state_from_mapping(state),
    )
    request.validate()
    return request


def _failure_run(request: AdapterRequest, failure_mode: str) -> WorkloadBoundRun:
    mapping = {
        "engine": request.engine.value,
        "engine_version": "adapter-failure",
        "corpus_sha256": request.corpus_sha256,
        "corpus_logical_bytes": request.corpus_logical_bytes,
        "workload_sha256": request.workload_sha256,
        "captured_at_utc": datetime.now(timezone.utc).isoformat().replace("+00:00", "Z"),
        "run_index": request.run_index,
        "system_state": request.system_state.to_dict(),
        "metrics": {},
        "failure_mode": _text(failure_mode[:1024], "failure_mode", 1024),
    }
    return bound_run_from_mapping(mapping)


def response_run_from_mapping(
    value: Mapping[str, object], request: AdapterRequest
) -> WorkloadBoundRun:
    required = {
        "protocol",
        "engine",
        "engine_version",
        "workload_sha256",
        "corpus_sha256",
        "corpus_logical_bytes",
        "captured_at_utc",
        "run_index",
        "system_state",
        "metrics",
        "failure_mode",
    }
    if set(value) != required:
        raise ValueError("adapter response fields mismatch")
    if value["protocol"] != PROTOCOL:
        raise ValueError("adapter response protocol mismatch")
    raw = dict(value)
    del raw["protocol"]
    bound = bound_run_from_mapping(raw)
    run = bound.run
    if run.engine != request.engine or run.run_index != request.run_index:
        raise ValueError("adapter response engine/run identity mismatch")
    if bound.workload_sha256 != request.workload_sha256:
        raise ValueError("adapter response workload identity mismatch")
    if (
        run.corpus_sha256 != request.corpus_sha256
        or run.corpus_logical_bytes != request.corpus_logical_bytes
    ):
        raise ValueError("adapter response corpus identity mismatch")
    if run.system_state.comparison_key() != request.system_state.comparison_key():
        raise ValueError("adapter response system state mismatch")
    return bound


def invoke_adapter(
    command: Sequence[str],
    request: AdapterRequest,
    timeout_seconds: float,
) -> WorkloadBoundRun:
    request.validate()
    if not command or any(not isinstance(part, str) or not part for part in command):
        raise ValueError("adapter command must contain non-empty argv strings")
    if (
        not math.isfinite(timeout_seconds)
        or timeout_seconds <= 0.0
        or timeout_seconds > 3600.0
    ):
        raise ValueError("adapter timeout must be in (0, 3600] seconds")
    rendered = (
        json.dumps(request.to_dict(), sort_keys=True, separators=(",", ":")) + "\n"
    )
    try:
        completed = subprocess.run(
            list(command),
            input=rendered,
            text=True,
            capture_output=True,
            timeout=timeout_seconds,
            check=False,
        )
    except subprocess.TimeoutExpired:
        return _failure_run(
            request, f"adapter timeout after {timeout_seconds:g} seconds"
        )
    except OSError as error:
        return _failure_run(
            request, f"adapter launch failed: {type(error).__name__}: {error}"
        )

    if completed.returncode != 0:
        diagnostic = (
            completed.stderr.strip() or completed.stdout.strip() or "no diagnostic"
        )
        return _failure_run(
            request, f"adapter exited {completed.returncode}: {diagnostic}"
        )
    try:
        response = json.loads(completed.stdout)
        if not isinstance(response, Mapping):
            raise ValueError("adapter response must be an object")
        return response_run_from_mapping(response, request)
    except (json.JSONDecodeError, ValueError, TypeError) as error:
        return _failure_run(request, f"adapter protocol failure: {error}")
