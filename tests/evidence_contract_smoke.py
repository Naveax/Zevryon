#!/usr/bin/env python3
from __future__ import annotations

from datetime import datetime, timezone
import json
import os
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from zevryon_platform.benchmark_metadata import capture_benchmark_metadata
from zevryon_platform.frame_latency_evidence import evaluate_frame_latencies, parse_frame_samples
from zevryon_platform.native_frame_certification import NativeFrameProbeSummary, build_native_frame_certification


def require(value: bool, message: str) -> None:
    if not value:
        raise RuntimeError(message)


def machine(*, physical: bool = True, thermal: str = "nominal"):
    env = {
        "ZEVRYON_PHYSICAL_DEVICE": "1" if physical else "0",
        "ZEVRYON_THERMAL_STATE": thermal,
        "ZEVRYON_THERMAL_C": "45.0,46.0",
        "ZEVRYON_BENCHMARK_RUN_LABEL": "ctest-evidence-smoke",
        "ZEVRYON_CPU_MODEL": "ctest-deterministic-cpu",
    }
    return capture_benchmark_metadata(
        env=env,
        captured_at=datetime(2026, 1, 1, tzinfo=timezone.utc),
        physical_ram_mib_override=16_384,
        logical_cpu_count_override=8,
    )


def probe(*, worker_starts: int = 2) -> NativeFrameProbeSummary:
    return NativeFrameProbeSummary(
        operation="zenith-tab-runtime-frame-probe",
        profile="desktop",
        frame_budget_us=8_330,
        warmup_samples=120,
        recorded_samples=1_000,
        visible_layouts=1_120,
        frame_overruns=0,
        prefetch_accepts=32,
        pool_thread_starts=worker_starts,
        pool_ready_peak_bytes=131_072,
    )


def main() -> int:
    # Keep ambient CI overrides from changing the deterministic profile.
    os.environ.pop("ZEVRYON_DEVICE_PROFILE", None)
    samples = parse_frame_samples("\n".join("1.000000000" for _ in range(1_000)))
    evidence = evaluate_frame_latencies(samples, machine())
    require(evidence.certified, "valid deterministic physical evidence was not certified")
    require(evidence.summary.sample_count == 1_000, "sample count changed")
    require(evidence.summary.p99_ms == 1.0, "nearest-rank P99 changed")
    require(len(evidence.summary.sample_sha256) == 64, "sample digest is not SHA-256")
    evidence_json = json.loads(evidence.to_json())
    require(evidence_json["machine"]["device_class"] == "desktop", "RAM-to-device profile mapping changed")
    require(evidence_json["checks"]["frame_latency_certified"] is True, "serialized evidence lost certified state")

    certification = build_native_frame_certification(
        probe(), evidence, requested_samples=1_000, requested_warmup=120
    )
    require(certification.certified, "valid native frame evidence was not certified")
    certification_json = json.loads(certification.to_json())
    require(certification_json["checks"]["native_frame_certified"] is True, "serialized native certification lost certified state")

    nonphysical = evaluate_frame_latencies(samples, machine(physical=False))
    require(not nonphysical.certified, "non-physical metadata unexpectedly passed certification")
    unstable = evaluate_frame_latencies(samples, machine(thermal="serious"))
    require(not unstable.certified, "unstable thermal state unexpectedly passed certification")
    invalid_worker_bound = build_native_frame_certification(
        probe(worker_starts=65), evidence, requested_samples=1_000, requested_warmup=120
    )
    require(not invalid_worker_bound.certified, "worker-bound violation unexpectedly passed native certification")

    print("Zevryon Python evidence contract smoke passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
