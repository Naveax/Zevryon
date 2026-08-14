#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT))

from zevryon_platform.performance_contract import DEVICE_PROFILES, DeviceClass  # noqa: E402

MIB = 1024 * 1024
TOUCH_BYTES = 4 * MIB
MIN_FILE_BACKED_PSS_FRACTION = 0.75
MIN_TOTAL_PSS_DELTA_BYTES = 2 * MIB
SMAPS_HEADER = re.compile(r"^[0-9a-fA-F]+-[0-9a-fA-F]+\s")


def read_rollup_pss_bytes(pid: int) -> int:
    for line in Path(f"/proc/{pid}/smaps_rollup").read_text(
        encoding="ascii", errors="ignore"
    ).splitlines():
        if line.startswith("Pss:"):
            return int(line.split()[1]) * 1024
    raise RuntimeError("Pss field missing from smaps_rollup")


def read_segment_pss_bytes(pid: int, segment_dir: Path) -> int:
    expected = str(segment_dir.resolve())
    selected = False
    total_kib = 0
    for line in Path(f"/proc/{pid}/smaps").read_text(
        encoding="utf-8", errors="ignore"
    ).splitlines():
        if SMAPS_HEADER.match(line):
            parts = line.split(maxsplit=5)
            mapped_path = parts[5] if len(parts) == 6 else ""
            selected = mapped_path.startswith(expected + "/")
            continue
        if selected and line.startswith("Pss:"):
            total_kib += int(line.split()[1])
    return total_kib * 1024


def run_checked(command: list[str]) -> dict:
    completed = subprocess.run(command, capture_output=True, text=True)
    if completed.returncode != 0:
        raise RuntimeError(
            f"command failed ({completed.returncode}): {' '.join(command)}\n"
            f"stdout:\n{completed.stdout}\n"
            f"stderr:\n{completed.stderr}"
        )
    return json.loads(completed.stdout)


def run_hold(
    probe: Path,
    store: Path,
    touch_bytes: int,
    cold_mib: int,
) -> dict:
    command = [
        str(probe),
        "hold",
        str(store),
        str(touch_bytes),
        str(cold_mib),
    ]
    process = subprocess.Popen(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        bufsize=1,
    )
    assert process.stdout is not None
    ready = process.stdout.readline().strip()
    if ready != "COLD_READY":
        stdout_tail, stderr = process.communicate()
        raise RuntimeError(
            f"cold PSS probe did not enter hold state; marker={ready!r}\n"
            f"stdout tail:\n{stdout_tail}\n"
            f"stderr:\n{stderr}"
        )

    peak_total_pss = 0
    peak_segment_pss = 0
    samples = 0
    sample_deadline = time.monotonic() + 0.65
    while time.monotonic() < sample_deadline and process.poll() is None:
        try:
            peak_total_pss = max(peak_total_pss, read_rollup_pss_bytes(process.pid))
            peak_segment_pss = max(
                peak_segment_pss,
                read_segment_pss_bytes(process.pid, store / "segments"),
            )
            samples += 1
        except (FileNotFoundError, ProcessLookupError):
            break
        time.sleep(0.01)

    stdout_tail, stderr = process.communicate()
    if process.returncode != 0:
        raise RuntimeError(
            f"cold PSS hold failed ({process.returncode})\n"
            f"stdout:\n{stdout_tail}\n"
            f"stderr:\n{stderr}"
        )
    if samples == 0 or peak_total_pss == 0:
        raise RuntimeError("no cold PSS hold samples were captured")

    return {
        "command": command,
        "samples": samples,
        "peak_total_pss_bytes": peak_total_pss,
        "peak_total_pss_mb": peak_total_pss / 1_000_000,
        "peak_segment_file_pss_bytes": peak_segment_pss,
        "peak_segment_file_pss_mb": peak_segment_pss / 1_000_000,
        "result": json.loads(stdout_tail),
        "stderr": stderr.strip(),
    }


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Certify that resident MassiveDoc cold-store pages are included in measured Linux PSS"
    )
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    if not Path("/proc/self/smaps_rollup").exists() or not Path("/proc/self/smaps").exists():
        raise RuntimeError("the M3 cold-store PSS gate requires Linux procfs smaps")

    profile = DEVICE_PROFILES[DeviceClass.LEGACY_PHONE]
    cold_mib = int(profile.cold_cache_mb)
    if cold_mib * MIB != TOUCH_BYTES:
        raise RuntimeError(
            "canonical legacy cold cache is expected to be exactly 4 MiB for this gate"
        )

    if args.work_dir.exists():
        shutil.rmtree(args.work_dir)
    args.work_dir.mkdir(parents=True)
    store = args.work_dir / "store"

    prepared = run_checked([str(args.probe), "prepare", str(store)])
    if prepared["logical_bytes"] < 2 * TOUCH_BYTES or prepared["segment_count"] != 1:
        raise RuntimeError("cold PSS fixture does not provide one physical segment >= 8 MiB")

    baseline = run_hold(args.probe, store, 0, cold_mib)
    cold = run_hold(args.probe, store, TOUCH_BYTES, cold_mib)

    baseline_total = int(baseline["peak_total_pss_bytes"])
    cold_total = int(cold["peak_total_pss_bytes"])
    cold_segment = int(cold["peak_segment_file_pss_bytes"])
    cold_result = cold["result"]

    minimum_file_pss = int(TOUCH_BYTES * MIN_FILE_BACKED_PSS_FRACTION)
    file_backed_visible = cold_segment >= minimum_file_pss
    total_delta_visible = cold_total >= baseline_total + MIN_TOTAL_PSS_DELTA_BYTES
    hot_warm_zero = int(cold_result["hot_warm_resident_bytes"]) == 0
    mapped_bounded = (
        int(cold_result["cold_mapped_bytes"]) == TOUCH_BYTES
        and int(cold_result["cold_peak_mapped_bytes"]) == TOUCH_BYTES
        and cold_segment <= TOUCH_BYTES
    )
    touch_accounted = int(cold_result["cold_touched_bytes"]) >= TOUCH_BYTES
    ledger_clean = bool(
        cold_result["cold_ledger_within_hard_limits"]
        and cold_result["cold_ledger_accounting_clean"]
    )
    target_ok = cold["peak_total_pss_mb"] <= profile.process_group_pss_target_mb
    hard_cap_ok = cold["peak_total_pss_mb"] <= profile.process_group_pss_hard_cap_mb

    checks = {
        "physical_cold_store_fixture": prepared["physical_bytes"] >= TOUCH_BYTES,
        "hot_warm_resident_zero": hot_warm_zero,
        "cold_mapping_hard_bounded": mapped_bounded,
        "cold_touch_accounted": touch_accounted,
        "cold_file_backed_pages_visible_in_smaps_pss": file_backed_visible,
        "cold_pages_raise_process_pss": total_delta_visible,
        "cold_window_ledger_clean": ledger_clean,
        "legacy_pss_target": target_ok,
        "legacy_pss_hard_cap": hard_cap_ok,
        "no_crash_or_oom": True,
    }
    checks["gate_pass"] = all(checks.values())
    if not checks["gate_pass"]:
        raise RuntimeError(f"M3 cold-store PSS gate failed: {checks}")

    report = {
        "schema": "zevryon.m3.cold-store-pss.v1",
        "device_profile": DeviceClass.LEGACY_PHONE.value,
        "fixture": prepared,
        "cold_window_bytes": TOUCH_BYTES,
        "baseline": baseline,
        "cold_resident": cold,
        "pss_delta_bytes": cold_total - baseline_total,
        "pss_delta_mb": (cold_total - baseline_total) / 1_000_000,
        "minimum_required_file_backed_pss_bytes": minimum_file_pss,
        "legacy_profile": {
            "process_group_pss_target_mb": profile.process_group_pss_target_mb,
            "process_group_pss_hard_cap_mb": profile.process_group_pss_hard_cap_mb,
            "hot_cache_mb": profile.hot_cache_mb,
            "warm_cache_mb": profile.warm_cache_mb,
            "cold_cache_mb": profile.cold_cache_mb,
        },
        "checks": checks,
        "scope": {
            "certifies": [
                "a bounded read-only file-backed MassiveDoc cold window",
                "zero hot/warm resident bytes during the cold hold state",
                "segment-file-backed resident pages are directly visible in /proc/<pid>/smaps Pss",
                "those cold pages increase process smaps_rollup PSS",
                "legacy-profile PSS target and hard-cap compliance while cold pages are resident",
            ],
            "does_not_claim": [
                "kernel page-cache bytes that are not mapped into the process",
                "unbounded mmap residency",
                "anonymous heap bytes as cold-store PSS",
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
