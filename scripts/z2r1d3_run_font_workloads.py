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

sys.path.insert(0, str(Path(__file__).resolve().parent))
from massivedoc_benchmark import read_process_sample  # noqa: E402

SCHEMA = "zevryon.z2r1d3.font-shaping-run.v1"
TIMING_KEYS = {
    "p50_ms",
    "p95_ms",
    "p99_ms",
    "maximum_ms",
    "elapsed_ms",
    "identities_per_second",
    "p50_throughput_mib_s",
}
REQUIRED_TESTS = [
    "font-catalog-fallback-tests",
    "font-discovery-generation-tests",
    "fontconfig-discovery-tests",
    "font-resource-sfnt-tests",
    "font-resource-integrity-tests",
    "font-content-identity-tests",
    "font-load-locator-tests",
    "font-file-loader-tests",
    "catalog-font-resource-resolver-tests",
    "verified-font-resource-tests",
    "verified-font-resource-cache-tests",
    "shaping-run-plan-tests",
    "harfbuzz-shaper-tests",
    "harfbuzz-verified-input-tests",
    "harfbuzz-verified-resource-tests",
    "catalog-harfbuzz-shaper-tests",
    "prepared-harfbuzz-face-tests",
    "prepared-harfbuzz-shaping-tests",
    "prepared-harfbuzz-face-cache-tests",
    "cached-catalog-harfbuzz-shaper-tests",
    "multi-run-harfbuzz-shaper-tests",
    "glyph-cluster-map-tests",
    "caret-boundary-map-tests",
]


def canonical_bytes(value: Any) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode(
        "utf-8"
    )


def sha256_value(value: Any) -> str:
    return hashlib.sha256(canonical_bytes(value)).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


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
            raise RuntimeError("benchmark output contains a non-object JSON value")
        values.append(value)
    if not values:
        raise RuntimeError("benchmark produced no JSON objects")
    return values


def run_measured(command: list[str]) -> dict[str, Any]:
    started = time.perf_counter()
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    peak_pss_bytes = 0
    stop = threading.Event()

    def monitor() -> None:
        nonlocal peak_pss_bytes
        while not stop.is_set():
            sample = read_process_sample(process.pid)
            pss = sample.get("pss_bytes")
            if isinstance(pss, int):
                peak_pss_bytes = max(peak_pss_bytes, pss)
            if process.poll() is not None:
                break
            time.sleep(0.002)

    thread = threading.Thread(target=monitor, daemon=True)
    thread.start()
    stdout, stderr = process.communicate()
    stop.set()
    thread.join(timeout=1.0)
    elapsed = time.perf_counter() - started
    if process.returncode != 0:
        raise RuntimeError(
            f"command failed ({process.returncode}): {' '.join(command)}\n{stderr.strip()}"
        )
    objects = parse_json_stream(stdout)
    return {
        "command": command,
        "seconds": elapsed,
        "peak_pss_bytes": peak_pss_bytes or None,
        "objects": objects,
        "stderr": stderr.strip(),
    }


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
    raise RuntimeError(f"benchmark output has neither {preferred} nor elapsed_ms")


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


def test_inventory(build_dir: Path) -> list[str]:
    completed = subprocess.run(
        ["ctest", "--test-dir", str(build_dir), "--show-only=json-v1"],
        capture_output=True,
        text=True,
        check=True,
    )
    payload = json.loads(completed.stdout)
    available = {str(item["name"]) for item in payload.get("tests", [])}
    missing = [name for name in REQUIRED_TESTS if name not in available]
    if missing:
        raise RuntimeError(f"required font/shaping tests are missing: {', '.join(missing)}")
    return REQUIRED_TESTS.copy()


def run_tests(build_dir: Path, names: list[str]) -> dict[str, Any]:
    expression = "^(" + "|".join(re.escape(name) for name in names) + ")$"
    started = time.perf_counter()
    completed = subprocess.run(
        [
            "ctest",
            "--test-dir",
            str(build_dir),
            "--output-on-failure",
            "--no-tests=error",
            "-R",
            expression,
        ],
        capture_output=True,
        text=True,
    )
    elapsed = time.perf_counter() - started
    if completed.returncode != 0:
        raise RuntimeError(
            f"font/shaping CTest suite failed ({completed.returncode})\n"
            f"{completed.stdout}\n{completed.stderr}"
        )
    return {
        "count": len(names),
        "names": names,
        "names_sha256": sha256_value(names),
        "wall_seconds": elapsed,
    }


def executable(build_dir: Path, name: str) -> Path:
    suffix = ".exe" if os.name == "nt" else ""
    path = build_dir / f"{name}{suffix}"
    if not path.is_file():
        raise RuntimeError(f"missing benchmark executable: {path}")
    return path


def workload_definitions(build_dir: Path, latin: Path, devanagari: Path) -> list[tuple[str, list[str]]]:
    return [
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
        (
            "harfbuzz_uncached",
            [
                str(executable(build_dir, "zevryon-harfbuzz-shaping-benchmark")),
                str(latin),
                str(devanagari),
            ],
        ),
        (
            "harfbuzz_verified_resource",
            [
                str(executable(build_dir, "zevryon-harfbuzz-shaping-benchmark")),
                str(latin),
                str(devanagari),
                "--verified-resource",
            ],
        ),
        (
            "prepared_harfbuzz",
            [str(executable(build_dir, "zevryon-prepared-harfbuzz-shaping-benchmark")), str(latin)],
        ),
        (
            "cached_catalog_harfbuzz",
            [
                str(executable(build_dir, "zevryon-cached-catalog-harfbuzz-shaping-benchmark")),
                str(latin),
            ],
        ),
        (
            "multi_run_harfbuzz",
            [str(executable(build_dir, "zevryon-multi-run-harfbuzz-shaping-benchmark")), str(latin)],
        ),
        (
            "glyph_cluster_map",
            [str(executable(build_dir, "zevryon-glyph-cluster-map-benchmark"))],
        ),
        (
            "caret_boundary_map",
            [str(executable(build_dir, "zevryon-caret-boundary-map-benchmark"))],
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
        int(sample["peak_pss_bytes"])
        for sample in samples
        if sample["peak_pss_bytes"] is not None
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
        "median_peak_pss_bytes": statistics.median(peaks) if peaks else None,
        "accounting_clean": all_boolean_fields(first_objects, "accounting_clean"),
        "within_hard_limits": all_boolean_fields(first_objects, "within_hard_limits"),
        "samples": samples,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--latin-font", type=Path, required=True)
    parser.add_argument("--devanagari-font", type=Path, required=True)
    parser.add_argument("--mode", choices=("baseline", "shadow"), required=True)
    parser.add_argument("--samples", type=int, default=3)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    if args.samples < 1:
        raise SystemExit("--samples must be positive")
    for font in (args.latin_font, args.devanagari_font):
        if not font.is_file() or font.stat().st_size == 0:
            raise SystemExit(f"font fixture is missing: {font}")

    started = time.perf_counter()
    names = test_inventory(args.build_dir)
    tests = run_tests(args.build_dir, names)
    workloads = [
        run_workload(name, command, args.samples)
        for name, command in workload_definitions(
            args.build_dir, args.latin_font, args.devanagari_font
        )
    ]
    report = {
        "schema": SCHEMA,
        "mode": args.mode,
        "fonts": {
            "latin": {
                "bytes": args.latin_font.stat().st_size,
                "sha256": sha256_file(args.latin_font),
            },
            "devanagari": {
                "bytes": args.devanagari_font.stat().st_size,
                "sha256": sha256_file(args.devanagari_font),
            },
        },
        "tests": tests,
        "workloads": workloads,
        "total_wall_seconds": time.perf_counter() - started,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps({
        "schema": SCHEMA,
        "mode": args.mode,
        "test_count": tests["count"],
        "workload_count": len(workloads),
        "total_wall_seconds": report["total_wall_seconds"],
    }, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
