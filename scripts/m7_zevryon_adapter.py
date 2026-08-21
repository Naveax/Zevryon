#!/usr/bin/env python3
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT))

from zevryon_platform.competitor_adapter import (  # noqa: E402
    PROTOCOL,
    adapter_request_from_mapping,
)
from zevryon_platform.competitor_lab import CORE_METRIC_NAMES, Engine  # noqa: E402
from zevryon_platform.competitor_workload import parse_canonical_workload  # noqa: E402
from zevryon_platform.performance_contract import DeviceClass  # noqa: E402

FRAME_METRICS = {"scroll_p99_ms", "maximum_normal_stall_ms"}
NATIVE_OPERATION_METRICS = {
    "first_viewport_preindexed_ms",
    "exact_search_warm_ms",
}
ADMITTED_METRICS = FRAME_METRICS | NATIVE_OPERATION_METRICS


def profile_for_campaign_ram(total_ram_mib: int) -> DeviceClass:
    # Mirrors the canonical performance-contract thresholds while deliberately
    # ignoring ZEVRYON_DEVICE_PROFILE; campaign evidence must follow recorded RAM.
    if total_ram_mib < 3072:
        return DeviceClass.LEGACY_PHONE
    if total_ram_mib < 6144:
        return DeviceClass.MID_PHONE
    if total_ram_mib < 12_288:
        return DeviceClass.MODERN_PHONE
    return DeviceClass.DESKTOP


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def nearest_rank(values: list[float], percentile: float) -> float:
    if not values or not (0.0 < percentile <= 1.0):
        raise ValueError("invalid nearest-rank input")
    ordered = sorted(values)
    rank = max(1, math.ceil(percentile * len(ordered)))
    return ordered[rank - 1]


def failure_response(request, engine_version: str, message: str) -> dict[str, object]:
    return {
        "protocol": PROTOCOL,
        "engine": request.engine.value,
        "engine_version": engine_version,
        "workload_sha256": request.workload_sha256,
        "corpus_sha256": request.corpus_sha256,
        "corpus_logical_bytes": request.corpus_logical_bytes,
        "captured_at_utc": utc_now(),
        "run_index": request.run_index,
        "system_state": request.system_state.to_dict(),
        "metrics": {},
        "failure_mode": message[:1024],
    }


def parse_frame_samples(path: Path, expected: int) -> list[float]:
    samples: list[float] = []
    for line_number, raw in enumerate(path.read_text(encoding="ascii").splitlines(), 1):
        text = raw.strip()
        if not text:
            continue
        try:
            value = float(text)
        except ValueError as error:
            raise ValueError(f"invalid frame sample at line {line_number}") from error
        if not math.isfinite(value) or value <= 0.0:
            raise ValueError(f"non-positive frame sample at line {line_number}")
        samples.append(value)
    if len(samples) != expected:
        raise ValueError(
            f"frame probe returned {len(samples)} samples, expected {expected}"
        )
    return samples


def measure_frame_metrics(
    frame_probe: Path,
    store_root: Path,
    request,
    timeout_seconds: float,
) -> dict[str, float]:
    workload = parse_canonical_workload(request.workload)
    if workload.sha256 != request.workload_sha256:
        raise ValueError("parsed canonical workload hash differs from adapter request")
    profile = profile_for_campaign_ram(request.system_state.physical_ram_mib).value
    with tempfile.TemporaryDirectory(prefix="zevryon-m7-frame-") as temporary:
        sample_path = Path(temporary) / "frames.ms"
        command = [
            str(frame_probe),
            str(store_root),
            profile,
            str(sample_path),
            str(workload.scroll_samples),
            str(workload.scroll_warmup),
            str(workload.viewport_width_px),
            str(workload.viewport_height_px),
            str(workload.overscan_px),
            str(workload.max_fragments),
            str(workload.scroll_step_px),
        ]
        completed = subprocess.run(
            command,
            capture_output=True,
            text=True,
            timeout=timeout_seconds,
            check=False,
        )
        if completed.returncode != 0:
            diagnostic = completed.stderr.strip() or completed.stdout.strip() or "no diagnostic"
            raise RuntimeError(
                f"native frame probe exited {completed.returncode}: {diagnostic}"
            )
        try:
            envelope = json.loads(completed.stdout)
        except json.JSONDecodeError as error:
            raise ValueError("native frame probe stdout is not one JSON object") from error
        if not isinstance(envelope, dict):
            raise ValueError("native frame probe envelope must be an object")
        if envelope.get("operation") != "zenith-tab-runtime-frame-probe":
            raise ValueError("unexpected native frame probe operation")
        if envelope.get("profile") != profile:
            raise ValueError("native frame probe profile mismatch")
        if envelope.get("warmup_samples") != workload.scroll_warmup:
            raise ValueError("native frame probe warmup mismatch")
        if envelope.get("recorded_samples") != workload.scroll_samples:
            raise ValueError("native frame probe sample-count mismatch")
        if envelope.get("visible_layouts") != (
            workload.scroll_warmup + workload.scroll_samples
        ):
            raise ValueError("native frame probe visible-layout count mismatch")
        samples = parse_frame_samples(sample_path, workload.scroll_samples)
    return {
        "scroll_p99_ms": nearest_rank(samples, 0.99),
        "maximum_normal_stall_ms": max(samples),
    }


def _run_json_command(command: list[str], timeout_seconds: float) -> dict[str, object]:
    completed = subprocess.run(
        command,
        capture_output=True,
        text=True,
        timeout=timeout_seconds,
        check=False,
    )
    if completed.returncode != 0:
        diagnostic = completed.stderr.strip() or completed.stdout.strip() or "no diagnostic"
        raise RuntimeError(
            f"native M7 probe exited {completed.returncode}: {diagnostic}"
        )
    try:
        envelope = json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        raise ValueError("native M7 probe stdout is not one JSON object") from error
    if not isinstance(envelope, dict):
        raise ValueError("native M7 probe envelope must be an object")
    return envelope


def measure_preindexed_metric(
    native_probe: Path,
    store_root: Path,
    request,
    timeout_seconds: float,
) -> float:
    workload = parse_canonical_workload(request.workload)
    profile = profile_for_campaign_ram(request.system_state.physical_ram_mib).value
    command = [
        str(native_probe),
        "preindexed",
        str(store_root),
        profile,
        str(workload.viewport_width_px),
        str(workload.viewport_height_px),
        str(workload.overscan_px),
        str(workload.max_fragments),
        "open-plus-first-layout-v1",
    ]
    envelope = _run_json_command(command, timeout_seconds)
    if envelope.get("operation") != "m7-preindexed-first-viewport":
        raise ValueError("unexpected M7 preindexed probe operation")
    if envelope.get("boundary") != "open-plus-first-layout-v1":
        raise ValueError("M7 preindexed timing boundary mismatch")
    if envelope.get("profile") != profile:
        raise ValueError("M7 preindexed profile mismatch")
    if envelope.get("used_checkpoint") is not True:
        raise ValueError("M7 preindexed viewport was not checkpoint-backed")
    fragments = envelope.get("fragments")
    milliseconds = envelope.get("milliseconds")
    if (
        not isinstance(fragments, int)
        or isinstance(fragments, bool)
        or fragments <= 0
        or not isinstance(milliseconds, (int, float))
        or isinstance(milliseconds, bool)
        or not math.isfinite(float(milliseconds))
        or float(milliseconds) <= 0.0
    ):
        raise ValueError("invalid M7 preindexed measurement envelope")
    return float(milliseconds)


def measure_warm_search_metric(
    native_probe: Path,
    store_root: Path,
    request,
    timeout_seconds: float,
) -> float:
    workload = parse_canonical_workload(request.workload)
    with tempfile.TemporaryDirectory(prefix="zevryon-m7-search-") as temporary:
        sample_path = Path(temporary) / "warm-search.ms"
        command = [
            str(native_probe),
            "warm-search",
            str(store_root),
            str(sample_path),
            str(workload.warm_search_trials),
            workload.search_query_utf8,
            "open-once-one-warmup-v1",
        ]
        envelope = _run_json_command(command, timeout_seconds)
        if envelope.get("operation") != "m7-warm-exact-search":
            raise ValueError("unexpected M7 warm-search probe operation")
        if envelope.get("boundary") != "open-once-one-warmup-v1":
            raise ValueError("M7 warm-search timing boundary mismatch")
        if envelope.get("trials") != workload.warm_search_trials:
            raise ValueError("M7 warm-search trial-count mismatch")
        if envelope.get("query_bytes") != len(
            workload.search_query_utf8.encode("utf-8")
        ):
            raise ValueError("M7 warm-search query-byte count mismatch")
        samples = parse_frame_samples(sample_path, workload.warm_search_trials)
    return nearest_rank(samples, 0.95)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run the fail-closed Zevryon M7 competitor adapter"
    )
    parser.add_argument("--frame-probe", type=Path, required=True)
    parser.add_argument("--native-probe", type=Path, required=True)
    parser.add_argument("--store-root", type=Path, required=True)
    parser.add_argument("--engine-version", required=True)
    parser.add_argument("--timeout-seconds", type=float, default=900.0)
    args = parser.parse_args()

    raw = json.loads(sys.stdin.read())
    if not isinstance(raw, dict):
        raise ValueError("adapter stdin must contain one JSON request object")
    request = adapter_request_from_mapping(raw)
    if request.engine != Engine.ZEVRYON:
        raise ValueError("Zevryon adapter received another engine identity")
    workload = parse_canonical_workload(request.workload)
    if workload.corpus_sha256 != request.corpus_sha256:
        raise ValueError("canonical workload corpus hash differs from request")
    if workload.corpus_logical_bytes != request.corpus_logical_bytes:
        raise ValueError("canonical workload corpus length differs from request")

    try:
        measured = measure_frame_metrics(
            args.frame_probe,
            args.store_root,
            request,
            args.timeout_seconds,
        )
        measured["first_viewport_preindexed_ms"] = measure_preindexed_metric(
            args.native_probe,
            args.store_root,
            request,
            args.timeout_seconds,
        )
        measured["exact_search_warm_ms"] = measure_warm_search_metric(
            args.native_probe,
            args.store_root,
            request,
            args.timeout_seconds,
        )
    except (OSError, subprocess.TimeoutExpired, RuntimeError, ValueError) as error:
        response = failure_response(
            request,
            args.engine_version,
            f"Zevryon native measurement primitive failed: {type(error).__name__}: {error}",
        )
        print(json.dumps(response, sort_keys=True))
        return 0

    missing = sorted(set(CORE_METRIC_NAMES) - ADMITTED_METRICS)
    response = failure_response(
        request,
        args.engine_version,
        (
            "Zevryon M7 measurement primitives incomplete; "
            f"missing={','.join(missing)}; "
            f"measured_preindexed_ms={measured['first_viewport_preindexed_ms']:.9f}; "
            f"measured_scroll_p99_ms={measured['scroll_p99_ms']:.9f}; "
            f"measured_maximum_normal_stall_ms={measured['maximum_normal_stall_ms']:.9f}; "
            f"measured_warm_search_ms={measured['exact_search_warm_ms']:.9f}"
        ),
    )
    print(json.dumps(response, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
