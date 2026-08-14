#!/usr/bin/env python3
from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import statistics
import subprocess
import sys
import time
from pathlib import Path
from typing import Any, Sequence

import z2r3e_validate_unicode_authority as authority

ROOT = authority.ROOT
FIXTURE_BYTES = 64 * 1024
PERFORMANCE_ITERATIONS = 256
MEMORY_ITERATIONS = 512
PERFORMANCE_PAIRS = 3
CHUNK_BYTES = 4096
CHUNK_MATRIX = (1, 2, 3, 4, 7, 64, 4096, 65536)
UNICODE_BUDGET_BYTES = 4 * 1024 * 1024
SEMANTIC_FIELDS = (
    "fixture_bytes",
    "chunk_bytes",
    "decoded_codepoints",
    "semantic_digest_fnv1a64",
    "unicode_hard_limit_bytes",
    "rejected_reservations",
    "accounting_errors",
    "within_hard_limits",
    "accounting_clean",
)


def executable_path(build: Path, platform_name: str) -> Path:
    if platform_name == "windows":
        return build / "Release" / "zevryon-unicode-benchmark.exe"
    return build / "zevryon-unicode-benchmark"


def parse_report(stdout: str, command: Sequence[str]) -> dict[str, Any]:
    lines = [line.strip() for line in stdout.splitlines() if line.strip()]
    if not lines:
        raise RuntimeError(f"benchmark returned no JSON: {subprocess.list2cmdline(list(command))}")
    report = json.loads(lines[-1])
    if report.get("schema") != "zevryon.unicode-benchmark.v2":
        raise RuntimeError(f"unexpected benchmark schema: {report.get('schema')!r}")
    return report


def run_report(executable: Path, iterations: int, chunk_bytes: int) -> dict[str, Any]:
    command = [
        str(executable),
        str(iterations),
        str(chunk_bytes),
        str(UNICODE_BUDGET_BYTES),
    ]
    completed = subprocess.run(
        command,
        cwd=ROOT,
        check=True,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    return parse_report(completed.stdout, command)


def linux_memory(pid: int) -> tuple[int, int | None]:
    rss = 0
    pss: int | None = None
    status = Path(f"/proc/{pid}/status")
    try:
        for line in status.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.startswith("VmHWM:"):
                rss = int(line.split()[1]) * 1024
                break
            if line.startswith("VmRSS:") and rss == 0:
                rss = int(line.split()[1]) * 1024
    except (FileNotFoundError, ProcessLookupError, PermissionError, ValueError):
        pass

    rollup = Path(f"/proc/{pid}/smaps_rollup")
    try:
        for line in rollup.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.startswith("Pss:"):
                pss = int(line.split()[1]) * 1024
                break
    except (FileNotFoundError, ProcessLookupError, PermissionError, ValueError):
        pass
    return rss, pss


class ProcessMemoryCounters(ctypes.Structure):
    _fields_ = [
        ("cb", ctypes.c_ulong),
        ("PageFaultCount", ctypes.c_ulong),
        ("PeakWorkingSetSize", ctypes.c_size_t),
        ("WorkingSetSize", ctypes.c_size_t),
        ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
        ("QuotaPagedPoolUsage", ctypes.c_size_t),
        ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
        ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
        ("PagefileUsage", ctypes.c_size_t),
        ("PeakPagefileUsage", ctypes.c_size_t),
    ]


def windows_peak_working_set(process: subprocess.Popen[str]) -> int:
    counters = ProcessMemoryCounters()
    counters.cb = ctypes.sizeof(counters)
    psapi = ctypes.WinDLL("psapi", use_last_error=True)
    result = psapi.GetProcessMemoryInfo(
        ctypes.c_void_p(int(process._handle)),  # type: ignore[attr-defined]
        ctypes.byref(counters),
        counters.cb,
    )
    if result == 0:
        return 0
    return int(counters.PeakWorkingSetSize)


def run_memory_report(
    executable: Path,
    platform_name: str,
    iterations: int,
    chunk_bytes: int,
) -> tuple[dict[str, Any], dict[str, int | None]]:
    command = [
        str(executable),
        str(iterations),
        str(chunk_bytes),
        str(UNICODE_BUDGET_BYTES),
    ]
    process = subprocess.Popen(
        command,
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    peak_rss = 0
    peak_pss: int | None = None
    while process.poll() is None:
        if platform_name == "linux":
            rss, pss = linux_memory(process.pid)
            peak_rss = max(peak_rss, rss)
            if pss is not None:
                peak_pss = pss if peak_pss is None else max(peak_pss, pss)
        else:
            peak_rss = max(peak_rss, windows_peak_working_set(process))
        time.sleep(0.001)

    if platform_name == "windows":
        peak_rss = max(peak_rss, windows_peak_working_set(process))
    stdout, stderr = process.communicate()
    if process.returncode != 0:
        raise RuntimeError(
            f"benchmark failed ({process.returncode}): {subprocess.list2cmdline(command)}\n{stderr}"
        )
    report = parse_report(stdout, command)
    return report, {
        "peak_rss_bytes": peak_rss,
        "peak_pss_bytes": peak_pss,
    }


def semantic_view(report: dict[str, Any]) -> dict[str, Any]:
    return {field: report[field] for field in SEMANTIC_FIELDS}


def require_mode(report: dict[str, Any], authoritative: bool) -> None:
    if bool(report.get("authority_mode")) != authoritative:
        raise RuntimeError(
            f"authority identity mismatch: expected {authoritative}, got {report.get('authority_mode')}"
        )
    if authoritative:
        if not bool(report.get("reverse_shadow_healthy")):
            raise RuntimeError("authority reverse shadow reported unhealthy")
        if int(report.get("reverse_shadow_mismatches", -1)) != 0:
            raise RuntimeError("authority reverse shadow reported a mismatch")


def require_pair_parity(
    baseline: dict[str, Any], authority_report: dict[str, Any], context: str
) -> None:
    require_mode(baseline, False)
    require_mode(authority_report, True)
    left = semantic_view(baseline)
    right = semantic_view(authority_report)
    if left != right:
        raise RuntimeError(
            f"semantic parity mismatch ({context}): baseline={left!r} authority={right!r}"
        )


def canonical_sha256(value: Any) -> str:
    payload = json.dumps(
        value,
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=False,
    ).encode("utf-8")
    return hashlib.sha256(payload).hexdigest()


def configure_and_build(platform_name: str) -> tuple[Path, Path]:
    baseline_build = ROOT / "build-z2r3eu-performance-default"
    authority_build = ROOT / "build-z2r3eu-performance-authority"
    authority.remove_build(baseline_build)
    authority.remove_build(authority_build)

    log_root = ROOT / "evidence" / "z2r3eu" / f"{platform_name}-performance-logs"
    authority.configure(
        baseline_build,
        authoritative=False,
        strict=False,
        hooks=False,
        log=log_root / "default-configure.log",
    )
    if (baseline_build / "rust-target").exists():
        raise RuntimeError("performance rollback build created rust-target")
    authority.build_targets(
        baseline_build,
        ("zevryon-unicode-benchmark",),
        log=log_root / "default-build.log",
    )
    if (baseline_build / "rust-target").exists():
        raise RuntimeError("performance rollback build used Cargo")

    authority.configure(
        authority_build,
        authoritative=True,
        strict=True,
        hooks=False,
        log=log_root / "authority-configure.log",
    )
    authority.build_targets(
        authority_build,
        ("zevryon-unicode-benchmark",),
        log=log_root / "authority-build.log",
    )

    baseline_executable = executable_path(baseline_build, platform_name)
    authority_executable = executable_path(authority_build, platform_name)
    if not baseline_executable.is_file() or not authority_executable.is_file():
        raise RuntimeError("benchmark executable was not produced")
    return baseline_executable, authority_executable


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--sha", required=True)
    parser.add_argument("--platform", choices=tuple(authority.SUPPORTED_HOSTS), required=True)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    authority.executable("git")
    authority.executable("cmake")
    authority.exact_head(args.sha)
    authority.require_clean_checkout()
    authority.require_supported_host(args.platform)

    started = time.time()
    baseline_executable, authority_executable = configure_and_build(args.platform)

    semantic_matrix: list[dict[str, Any]] = []
    for chunk_bytes in CHUNK_MATRIX:
        baseline = run_report(baseline_executable, 10, chunk_bytes)
        promoted = run_report(authority_executable, 10, chunk_bytes)
        require_pair_parity(baseline, promoted, f"chunk={chunk_bytes}")
        semantic_matrix.append(
            {
                "chunk_bytes": chunk_bytes,
                "semantic": semantic_view(baseline),
            }
        )

    pairs: list[dict[str, Any]] = []
    ratios: list[float] = []
    for pair_index in range(PERFORMANCE_PAIRS):
        if pair_index % 2 == 0:
            baseline = run_report(baseline_executable, PERFORMANCE_ITERATIONS, CHUNK_BYTES)
            promoted = run_report(authority_executable, PERFORMANCE_ITERATIONS, CHUNK_BYTES)
            order = "baseline-authority"
        else:
            promoted = run_report(authority_executable, PERFORMANCE_ITERATIONS, CHUNK_BYTES)
            baseline = run_report(baseline_executable, PERFORMANCE_ITERATIONS, CHUNK_BYTES)
            order = "authority-baseline"
        require_pair_parity(baseline, promoted, f"performance-pair={pair_index}")
        ratio = float(promoted["p50_ms"]) / float(baseline["p50_ms"])
        ratios.append(ratio)
        pairs.append(
            {
                "pair": pair_index + 1,
                "order": order,
                "baseline": baseline,
                "authority": promoted,
                "p50_authority_to_baseline_ratio": ratio,
            }
        )

    baseline_memory_report, baseline_memory = run_memory_report(
        baseline_executable, args.platform, MEMORY_ITERATIONS, CHUNK_BYTES
    )
    authority_memory_report, authority_memory = run_memory_report(
        authority_executable, args.platform, MEMORY_ITERATIONS, CHUNK_BYTES
    )
    require_pair_parity(
        baseline_memory_report, authority_memory_report, "memory-sample"
    )

    baseline_rss = int(baseline_memory["peak_rss_bytes"] or 0)
    authority_rss = int(authority_memory["peak_rss_bytes"] or 0)
    if baseline_rss <= 0 or authority_rss <= 0:
        raise RuntimeError("peak RSS/working-set measurement was unavailable")

    baseline_pss = baseline_memory["peak_pss_bytes"]
    authority_pss = authority_memory["peak_pss_bytes"]
    if args.platform == "linux" and (baseline_pss is None or authority_pss is None):
        raise RuntimeError("Linux peak PSS measurement was unavailable")

    report: dict[str, Any] = {
        "schema": "zevryon.z2r3eu.performance-validation.v1",
        "commit_sha": args.sha,
        "platform": args.platform,
        "compiler": args.compiler,
        "fixture_bytes": FIXTURE_BYTES,
        "performance_iterations_per_sample": PERFORMANCE_ITERATIONS,
        "logical_bytes_per_performance_sample": FIXTURE_BYTES * PERFORMANCE_ITERATIONS,
        "performance_pairs": PERFORMANCE_PAIRS,
        "performance_chunk_bytes": CHUNK_BYTES,
        "semantic_chunk_matrix": list(CHUNK_MATRIX),
        "semantic_matrix": semantic_matrix,
        "semantic_matrix_sha256": canonical_sha256(semantic_matrix),
        "paired_samples": pairs,
        "p50_authority_to_baseline_ratios": ratios,
        "p50_authority_to_baseline_ratio_median": statistics.median(ratios),
        "memory_iterations": MEMORY_ITERATIONS,
        "memory_logical_bytes": FIXTURE_BYTES * MEMORY_ITERATIONS,
        "baseline_memory": baseline_memory,
        "authority_memory": authority_memory,
        "peak_rss_authority_to_baseline_ratio": authority_rss / baseline_rss,
        "peak_rss_delta_bytes": authority_rss - baseline_rss,
        "peak_pss_authority_to_baseline_ratio": (
            None
            if baseline_pss is None or authority_pss is None
            else int(authority_pss) / int(baseline_pss)
        ),
        "peak_pss_delta_bytes": (
            None
            if baseline_pss is None or authority_pss is None
            else int(authority_pss) - int(baseline_pss)
        ),
        "semantic_parity": True,
        "authority_reverse_shadow_healthy": True,
        "threshold_policy": "report-only-no-hosted-runner-latency-threshold",
        "duration_seconds": round(time.time() - started, 3),
    }
    report["canonical_payload_sha256"] = canonical_sha256(report)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    final_bytes = (json.dumps(report, indent=2, sort_keys=True) + "\n").encode("utf-8")
    args.output.write_bytes(final_bytes)
    file_sha = hashlib.sha256(final_bytes).hexdigest()
    sidecar = args.output.with_suffix(args.output.suffix + ".sha256")
    sidecar.write_text(f"{file_sha}  {args.output.name}\n", encoding="ascii")
    print(f"performance summary: {args.output}")
    print(f"performance SHA-256: {file_sha}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
