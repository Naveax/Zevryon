#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT))

from zevryon_platform.performance_contract import (  # noqa: E402
    DEVICE_PROFILES,
    TITAN_WORST_CASE,
    DeviceClass,
)

MIB = 1024 * 1024
RESIDENT_TOUCH_BYTES = 4 * MIB
SPARSE_ALLOCATION_LIMIT_BYTES = 64 * MIB
PSS_DELTA_FLOOR_MB = 1.0


def read_pss_bytes(pid: int) -> int | None:
    try:
        for line in Path(f"/proc/{pid}/smaps_rollup").read_text(
            encoding="ascii", errors="ignore"
        ).splitlines():
            if line.startswith("Pss:"):
                return int(line.split()[1]) * 1024
    except (FileNotFoundError, PermissionError, ProcessLookupError, ValueError):
        return None
    return None


def run_checked(command: list[str]) -> dict:
    completed = subprocess.run(command, capture_output=True, text=True)
    if completed.returncode != 0:
        raise RuntimeError(
            f"command failed ({completed.returncode}): {' '.join(command)}\n"
            f"stdout:\n{completed.stdout}\n"
            f"stderr:\n{completed.stderr}"
        )
    return json.loads(completed.stdout)


def run_measured(command: list[str]) -> dict:
    started = time.perf_counter()
    process = subprocess.Popen(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    peak_pss_bytes = 0
    samples = 0
    while process.poll() is None:
        pss = read_pss_bytes(process.pid)
        if pss is not None:
            peak_pss_bytes = max(peak_pss_bytes, pss)
            samples += 1
        time.sleep(0.002)
    stdout, stderr = process.communicate()
    elapsed = time.perf_counter() - started
    final_pss = read_pss_bytes(process.pid)
    if final_pss is not None:
        peak_pss_bytes = max(peak_pss_bytes, final_pss)
        samples += 1
    if process.returncode != 0:
        raise RuntimeError(
            f"measured command failed ({process.returncode}): {' '.join(command)}\n"
            f"stdout:\n{stdout}\n"
            f"stderr:\n{stderr}"
        )
    if peak_pss_bytes == 0 or samples == 0:
        raise RuntimeError("no Linux PSS sample was captured for the open probe")
    return {
        "command": command,
        "seconds": elapsed,
        "peak_pss_bytes": peak_pss_bytes,
        "peak_pss_mb": peak_pss_bytes / 1_000_000,
        "samples": samples,
        "result": json.loads(stdout),
        "stderr": stderr.strip(),
    }


def allocated_bytes(root: Path) -> int:
    total = 0
    for path in root.rglob("*"):
        if not path.is_file():
            continue
        stat = path.stat()
        blocks = getattr(stat, "st_blocks", None)
        if blocks is None:
            raise RuntimeError("st_blocks is unavailable; Linux sparse allocation cannot be proven")
        total += int(blocks) * 512
    return total


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Certify the M3 4 GiB legacy-profile StoreReader open gate"
    )
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    if not Path("/proc/self/smaps_rollup").exists():
        raise RuntimeError("the M3 legacy open PSS gate requires Linux procfs")

    profile = DEVICE_PROFILES[DeviceClass.LEGACY_PHONE]
    expected_logical_bytes = TITAN_WORST_CASE.logical_utf8_bytes
    if expected_logical_bytes != 4 * 1024 * MIB:
        raise RuntimeError("canonical Titan logical payload is no longer exactly 4 GiB")
    if profile.hot_cache_mb + profile.warm_cache_mb > 16:
        raise RuntimeError("legacy hot+warm cache exceeds StoreReader's 16 MiB resident cache cap")

    if args.work_dir.exists():
        shutil.rmtree(args.work_dir)
    args.work_dir.mkdir(parents=True)
    store = args.work_dir / "store"

    prepared = run_checked([str(args.probe), "prepare", str(store)])
    if prepared["logical_bytes"] != expected_logical_bytes:
        raise RuntimeError("sparse fixture logical payload differs from canonical 4 GiB")
    sparse_bytes = allocated_bytes(store)
    sparse_ok = sparse_bytes <= SPARSE_ALLOCATION_LIMIT_BYTES
    if not sparse_ok:
        raise RuntimeError(
            f"sparse fixture allocated {sparse_bytes} bytes, expected <= "
            f"{SPARSE_ALLOCATION_LIMIT_BYTES}"
        )

    open_base = [
        str(args.probe),
        "open",
        str(store),
    ]
    baseline = run_measured(
        open_base
        + [
            "0",
            str(profile.hot_cache_mb),
            str(profile.warm_cache_mb),
        ]
    )
    resident = run_measured(
        open_base
        + [
            str(RESIDENT_TOUCH_BYTES),
            str(profile.hot_cache_mb),
            str(profile.warm_cache_mb),
        ]
    )

    for name, measurement in (("baseline", baseline), ("resident", resident)):
        result = measurement["result"]
        if result["logical_bytes"] != expected_logical_bytes:
            raise RuntimeError(f"{name} open returned the wrong logical byte count")
        if result["segment_count"] != prepared["segment_count"]:
            raise RuntimeError(f"{name} open returned the wrong segment count")
        if not result["head_tail_slice_ok"]:
            raise RuntimeError(f"{name} did not validate both head and tail slices")
        if not result["cache_ledger_within_hard_limits"]:
            raise RuntimeError(f"{name} exceeded the cache ledger hard limit")
        if not result["cache_ledger_accounting_clean"]:
            raise RuntimeError(f"{name} left cache ledger accounting dirty")

    baseline_pss = float(baseline["peak_pss_mb"])
    resident_pss = float(resident["peak_pss_mb"])
    target_ok = (
        baseline_pss <= profile.process_group_pss_target_mb
        and resident_pss <= profile.process_group_pss_target_mb
    )
    hard_cap_ok = (
        baseline_pss <= profile.process_group_pss_hard_cap_mb
        and resident_pss <= profile.process_group_pss_hard_cap_mb
    )
    resident_cache_bytes = int(resident["result"]["cache_resident_bytes"])
    resident_payload_accounted = (
        resident_cache_bytes >= RESIDENT_TOUCH_BYTES
        and resident_pss >= resident_cache_bytes / 1_000_000
        and resident_pss >= baseline_pss + PSS_DELTA_FLOOR_MB
    )

    checks = {
        "exact_4gib_logical_payload": prepared["logical_bytes"] == expected_logical_bytes,
        "sparse_fixture_bounded_allocation": sparse_ok,
        "head_and_tail_bounded_reads": bool(
            baseline["result"]["head_tail_slice_ok"]
            and resident["result"]["head_tail_slice_ok"]
        ),
        "legacy_pss_target": target_ok,
        "legacy_pss_hard_cap": hard_cap_ok,
        "resident_store_payload_visible_in_pss": resident_payload_accounted,
        "cache_ledger_clean": bool(
            resident["result"]["cache_ledger_accounting_clean"]
            and baseline["result"]["cache_ledger_accounting_clean"]
        ),
        "no_crash_or_oom": True,
    }
    checks["gate_pass"] = all(checks.values())
    if not checks["gate_pass"]:
        raise RuntimeError(f"M3 legacy 4 GiB open gate failed: {checks}")

    report = {
        "schema": "zevryon.m3.legacy-4gib-open.v1",
        "device_profile": DeviceClass.LEGACY_PHONE.value,
        "logical_bytes": expected_logical_bytes,
        "logical_records": prepared["logical_records"],
        "segment_bytes": prepared["segment_bytes"],
        "segment_count": prepared["segment_count"],
        "legacy_profile": {
            "process_group_pss_target_mb": profile.process_group_pss_target_mb,
            "process_group_pss_hard_cap_mb": profile.process_group_pss_hard_cap_mb,
            "hot_cache_mb": profile.hot_cache_mb,
            "warm_cache_mb": profile.warm_cache_mb,
            "cold_cache_mb": profile.cold_cache_mb,
            "cold_tier_residency": "disk-only",
        },
        "sparse_fixture": {
            "logical_segment_bytes": expected_logical_bytes,
            "allocated_bytes": sparse_bytes,
            "allocation_limit_bytes": SPARSE_ALLOCATION_LIMIT_BYTES,
        },
        "baseline_open": baseline,
        "resident_touch_open": resident,
        "resident_touch_bytes": RESIDENT_TOUCH_BYTES,
        "checks": checks,
        "scope": {
            "certifies": [
                "StoreReader canonical-generation open at exactly 4 GiB logical payload",
                "bounded head and tail reads across the 4 GiB store",
                "legacy-profile PSS target and hard-cap compliance",
                "resident immutable-store payload is reflected in measured process PSS",
            ],
            "does_not_certify": [
                "full 4 GiB payload SHA verification",
                "8,388,608-record Titan envelope",
                "first-viewport-before-background-index completion",
            ],
        },
    }

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(json.dumps(report, indent=2, sort_keys=True))
    shutil.rmtree(args.work_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
