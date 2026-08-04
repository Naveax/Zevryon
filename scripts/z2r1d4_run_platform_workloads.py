#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import statistics
import subprocess
import sys
import threading
import time
from typing import Any

import psutil

SCHEMA = "zevryon.z2r1d4.platform-run.v1"
TIMING_KEYS = {
    "p50_ms",
    "p95_ms",
    "p99_ms",
    "maximum_ms",
    "elapsed_ms",
    "identities_per_second",
    "p50_throughput_mib_s",
}
COMMON_TESTS = [
    "font-catalog-fallback-tests",
    "font-discovery-generation-tests",
    "font-resource-sfnt-tests",
    "font-resource-integrity-tests",
    "font-content-identity-tests",
    "font-load-locator-tests",
    "shaping-run-plan-tests",
    "resource-ledger-tests",
]
PLATFORM_TEST = {
    "windows": "directwrite-discovery-tests",
    "macos": "coretext-discovery-tests",
}
ADAPTER = {
    "windows": "directwrite",
    "macos": "coretext",
}


def canonical_bytes(value: Any) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode(
        "utf-8"
    )


def sha256_value(value: Any) -> str:
    return hashlib.sha256(canonical_bytes(value)).hexdigest()


def scrub_timing(value: Any) -> Any:
    if isinstance(value, dict):
        return {
            key: scrub_timing(item)
            for key, item in sorted(value.items())
            if key not in TIMING_KEYS
        }
    if isinstance(value, list):
        return [scrub_timing(item) for item in value]
    return value


def parse_json_stream(text: str) -> list[dict[str, Any]]:
    decoder = json.JSONDecoder()
    index = 0
    values: list[dict[str, Any]] = []
    while True:
        while index < len(text) and text[index].isspace():
            index += 1
        if index >= len(text):
            break
        value, index = decoder.raw_decode(text, index)
        if not isinstance(value, dict):
            raise RuntimeError("program output contains a non-object JSON value")
        values.append(value)
    if not values:
        raise RuntimeError("program produced no JSON objects")
    return values


def process_rss_bytes(process: psutil.Process) -> int:
    total = 0
    try:
        total += int(process.memory_info().rss)
    except (psutil.Error, OSError):
        return 0
    try:
        for child in process.children(recursive=True):
            try:
                total += int(child.memory_info().rss)
            except (psutil.Error, OSError):
                continue
    except (psutil.Error, OSError):
        pass
    return total


def run_measured(command: list[str]) -> dict[str, Any]:
    started = time.perf_counter()
    child = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    monitored = psutil.Process(child.pid)
    peak_rss_bytes = 0
    stop = threading.Event()

    def monitor() -> None:
        nonlocal peak_rss_bytes
        while not stop.is_set():
            peak_rss_bytes = max(peak_rss_bytes, process_rss_bytes(monitored))
            if child.poll() is not None:
                break
            time.sleep(0.002)

    thread = threading.Thread(target=monitor, daemon=True)
    thread.start()
    stdout, stderr = child.communicate()
    stop.set()
    thread.join(timeout=1.0)
    elapsed = time.perf_counter() - started
    if child.returncode != 0:
        raise RuntimeError(
            f"command failed ({child.returncode}): {' '.join(command)}\n"
            f"--- stdout ---\n{stdout.strip()}\n"
            f"--- stderr ---\n{stderr.strip()}"
        )
    return {
        "command": command,
        "seconds": elapsed,
        "peak_rss_bytes": peak_rss_bytes or None,
        "objects": parse_json_stream(stdout),
        "stderr": stderr.strip(),
    }


def run_json(command: list[str]) -> dict[str, Any]:
    completed = subprocess.run(command, capture_output=True, text=True)
    if completed.returncode != 0:
        raise RuntimeError(
            f"command failed ({completed.returncode}): {' '.join(command)}\n"
            f"--- stdout ---\n{completed.stdout.strip()}\n"
            f"--- stderr ---\n{completed.stderr.strip()}"
        )
    objects = parse_json_stream(completed.stdout)
    if len(objects) != 1:
        raise RuntimeError("expected exactly one JSON object")
    return objects[0]


def finite_numbers(objects: list[dict[str, Any]], key: str) -> list[float]:
    values: list[float] = []
    for item in objects:
        raw = item.get(key)
        if raw is None:
            continue
        number = float(raw)
        if math.isfinite(number) and number >= 0.0:
            values.append(number)
    return values


def internal_latency(objects: list[dict[str, Any]], preferred: str) -> float:
    values = finite_numbers(objects, preferred)
    if values:
        return max(values)
    values = finite_numbers(objects, "elapsed_ms")
    if values:
        return max(values)
    raise RuntimeError(f"program output has neither {preferred} nor elapsed_ms")


def all_boolean_fields(objects: list[dict[str, Any]], key: str) -> bool:
    def visit(value: Any) -> bool:
        if isinstance(value, dict):
            for field, item in value.items():
                if field == key and item is not True:
                    return False
                if not visit(item):
                    return False
        elif isinstance(value, list):
            for item in value:
                if not visit(item):
                    return False
        return True

    return visit(objects)


def executable(build_dir: Path, name: str) -> Path:
    suffix = ".exe" if os.name == "nt" else ""
    candidates = [
        build_dir / f"{name}{suffix}",
        build_dir / "Release" / f"{name}{suffix}",
        build_dir / "RelWithDebInfo" / f"{name}{suffix}",
    ]
    for path in candidates:
        if path.is_file():
            return path
    raise RuntimeError(
        f"missing executable {name}; checked: " + ", ".join(str(path) for path in candidates)
    )


def ctest_prefix(build_dir: Path) -> list[str]:
    command = ["ctest", "--test-dir", str(build_dir)]
    if os.name == "nt":
        command.extend(["-C", "Release"])
    return command


def test_inventory(build_dir: Path, platform_name: str) -> list[str]:
    completed = subprocess.run(
        ctest_prefix(build_dir) + ["--show-only=json-v1"],
        capture_output=True,
        text=True,
        check=True,
    )
    payload = json.loads(completed.stdout)
    available = {str(item["name"]) for item in payload.get("tests", [])}
    required = COMMON_TESTS + [PLATFORM_TEST[platform_name]]
    missing = [name for name in required if name not in available]
    if missing:
        raise RuntimeError(f"required platform tests are missing: {', '.join(missing)}")
    return required


def run_tests(build_dir: Path, names: list[str]) -> dict[str, Any]:
    expression = "^(" + "|".join(re.escape(name) for name in names) + ")$"
    started = time.perf_counter()
    completed = subprocess.run(
        ctest_prefix(build_dir)
        + ["--output-on-failure", "--no-tests=error", "-R", expression],
        capture_output=True,
        text=True,
    )
    elapsed = time.perf_counter() - started
    if completed.returncode != 0:
        raise RuntimeError(
            f"platform CTest suite failed ({completed.returncode})\n"
            f"{completed.stdout}\n{completed.stderr}"
        )
    return {
        "count": len(names),
        "names": names,
        "names_sha256": sha256_value(names),
        "wall_seconds": elapsed,
    }


def workload_definitions(build_dir: Path) -> list[tuple[str, list[str]]]:
    return [
        (
            "platform_discovery",
            [str(executable(build_dir, "zevryon-z2r1d4-platform-probe"))],
        ),
        (
            "font_content_identity",
            [str(executable(build_dir, "zevryon-font-content-identity-benchmark"))],
        ),
        (
            "font_load_locator",
            [str(executable(build_dir, "zevryon-font-load-locator-benchmark"))],
        ),
        (
            "shaping_run_plan",
            [
                str(executable(build_dir, "zevryon-shaping-run-plan-benchmark")),
                "256",
                "49152",
            ],
        ),
    ]


def run_workload(name: str, command: list[str], sample_count: int) -> dict[str, Any]:
    samples = [run_measured(command) for _ in range(sample_count)]
    semantic_hashes = [sha256_value(scrub_timing(sample["objects"])) for sample in samples]
    if len(set(semantic_hashes)) != 1:
        raise RuntimeError(f"{name} produced non-deterministic semantic output")
    p50_values = [internal_latency(sample["objects"], "p50_ms") for sample in samples]
    p95_values = [internal_latency(sample["objects"], "p95_ms") for sample in samples]
    wall_values = [float(sample["seconds"]) for sample in samples]
    peaks = [
        int(sample["peak_rss_bytes"])
        for sample in samples
        if sample["peak_rss_bytes"] is not None
    ]
    first_objects = samples[0]["objects"]
    return {
        "name": name,
        "sample_count": sample_count,
        "semantic_sha256": semantic_hashes[0],
        "semantic_objects": scrub_timing(first_objects),
        "median_p50_ms": statistics.median(p50_values),
        "median_p95_ms": statistics.median(p95_values),
        "median_wall_seconds": statistics.median(wall_values),
        "median_peak_rss_bytes": statistics.median(peaks) if peaks else None,
        "accounting_clean": all_boolean_fields(first_objects, "accounting_clean"),
        "within_hard_limits": all_boolean_fields(first_objects, "within_hard_limits"),
        "samples": samples,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--platform", choices=("windows", "macos"), required=True)
    parser.add_argument("--mode", choices=("baseline", "shadow"), required=True)
    parser.add_argument("--samples", type=int, default=3)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    if args.samples < 1:
        raise SystemExit("--samples must be positive")
    expected_platform = "windows" if os.name == "nt" else "macos" if sys.platform == "darwin" else None
    if expected_platform != args.platform:
        raise SystemExit(
            f"runner platform mismatch: requested {args.platform}, actual {expected_platform}"
        )

    started = time.perf_counter()
    names = test_inventory(args.build_dir, args.platform)
    tests = run_tests(args.build_dir, names)
    workloads = [
        run_workload(name, command, args.samples)
        for name, command in workload_definitions(args.build_dir)
    ]
    probe = None
    if args.mode == "shadow":
        probe = run_json(
            [
                str(executable(args.build_dir, "zevryon-resource-ledger-tests")),
                "--emit-rust-shadow-workload-probe",
            ]
        )

    report = {
        "schema": SCHEMA,
        "platform": args.platform,
        "adapter": ADAPTER[args.platform],
        "mode": args.mode,
        "tests": tests,
        "workloads": workloads,
        "rust_shadow_probe": probe,
        "total_wall_seconds": time.perf_counter() - started,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(
        json.dumps(
            {
                "schema": SCHEMA,
                "platform": args.platform,
                "mode": args.mode,
                "test_count": tests["count"],
                "workload_count": len(workloads),
                "total_wall_seconds": report["total_wall_seconds"],
            },
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
