#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from zevryon_platform.performance_contract import DEVICE_PROFILES, DeviceClass

DECIMAL_MB = 1_000_000
DEFAULT_BLOCK_BYTES = 64 * 1024
DEFAULT_HOT_BLOCK_BYTES = 256 * 1024
DEFAULT_WARM_BLOCK_BYTES = 512 * 1024
DEFAULT_CHECKPOINT_BYTES = 1_000_000
DEFAULT_SOURCE_WINDOW_BYTES = 512 * 1024
MAX_BLOCK_CACHE_BYTES = 16 * 1024 * 1024
MAX_COLD_32_BYTES = 8 * 1024 * 1024
MAX_COLD_64_BYTES = 16 * 1024 * 1024


class TestFailure(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise TestFailure(message)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", type=Path, required=True)
    args = parser.parse_args()
    try:
        probe = args.probe.resolve()
        require(probe.is_file(), f"profile policy probe not found: {probe}")
        completed = subprocess.run(
            [str(probe)],
            text=True,
            encoding="utf-8",
            errors="strict",
            capture_output=True,
            check=False,
            timeout=20.0,
        )
        require(
            completed.returncode == 0,
            f"profile policy probe failed: stdout={completed.stdout!r}; stderr={completed.stderr!r}",
        )
        document = json.loads(completed.stdout)
        require(document.get("schema") == "zevryon.m8.profile-runtime-policy.v1", "policy schema drifted")
        pointer_bits = document.get("pointer_bits")
        require(type(pointer_bits) is int and pointer_bits >= 32, "invalid pointer width receipt")
        profiles = document.get("profiles")
        require(isinstance(profiles, list) and len(profiles) == len(DeviceClass), "policy profile count drifted")
        by_name = {item.get("name"): item for item in profiles if isinstance(item, dict)}
        require(set(by_name) == {item.value for item in DeviceClass}, "policy profile set drifted")

        for device in DeviceClass:
            py = DEVICE_PROFILES[device]
            cpp = by_name[device.value]
            expected = {
                "minimum_physical_ram_mib": py.minimum_physical_ram_mib,
                "process_group_pss_target_mb": py.process_group_pss_target_mb,
                "process_group_pss_hard_cap_mb": py.process_group_pss_hard_cap_mb,
                "hot_cache_mb": py.hot_cache_mb,
                "warm_cache_mb": py.warm_cache_mb,
                "cold_cache_mb": py.cold_cache_mb,
                "first_viewport_preindexed_ms": py.first_viewport_preindexed_ms,
                "first_viewport_streaming_ms": py.first_viewport_streaming_ms,
                "scroll_p99_ms": py.scroll_p99_ms,
                "maximum_normal_stall_ms": py.maximum_normal_stall_ms,
                "exact_search_warm_ms": py.exact_search_warm_ms,
                "exact_search_cold_ms": py.exact_search_cold_ms,
                "mutation_p95_us": py.mutation_p95_us,
                "copy_throughput_mib_s": py.copy_throughput_mib_s,
            }
            for key, value in expected.items():
                require(cpp.get(key) == value, f"{device.value}: C++/Python contract drift for {key}")

            runtime = cpp.get("runtime")
            budgets = cpp.get("budgets")
            require(isinstance(runtime, dict), f"{device.value}: runtime receipt missing")
            require(isinstance(budgets, dict), f"{device.value}: budget receipt missing")

            hot_budget = py.hot_cache_mb * DECIMAL_MB
            warm_budget = py.warm_cache_mb * DECIMAL_MB
            cold_budget = py.cold_cache_mb * DECIMAL_MB
            hot_block = min(DEFAULT_HOT_BLOCK_BYTES, hot_budget)
            hot_layout = hot_budget - hot_block
            warm_block = min(DEFAULT_WARM_BLOCK_BYTES, warm_budget)
            warm_remaining = warm_budget - warm_block
            checkpoint = min(DEFAULT_CHECKPOINT_BYTES, warm_remaining)
            warm_remaining -= checkpoint
            source_window = min(DEFAULT_SOURCE_WINDOW_BYTES, warm_remaining)
            warm_remaining -= source_window
            cold_max = MAX_COLD_32_BYTES if pointer_bits <= 32 else MAX_COLD_64_BYTES
            cold_window = min(cold_budget, cold_max)

            require(runtime.get("block_bytes") == DEFAULT_BLOCK_BYTES, f"{device.value}: block size drifted")
            require(runtime.get("block_hot_bytes") == hot_block, f"{device.value}: hot block budget drifted")
            require(runtime.get("block_warm_bytes") == warm_block, f"{device.value}: warm block budget drifted")
            require(runtime.get("layout_cache_bytes") == hot_layout, f"{device.value}: layout cache budget drifted")
            require(runtime.get("checkpoint_cache_bytes") == checkpoint, f"{device.value}: checkpoint budget drifted")
            require(runtime.get("source_window_cache_bytes") == source_window, f"{device.value}: source-window budget drifted")
            require(runtime.get("cold_window_bytes") == cold_window, f"{device.value}: cold window budget drifted")

            require(budgets.get("hot_budget_bytes") == hot_budget, f"{device.value}: hot profile budget drifted")
            require(budgets.get("hot_block_cache_bytes") == hot_block, f"{device.value}: hot block receipt drifted")
            require(budgets.get("hot_layout_cache_bytes") == hot_layout, f"{device.value}: hot layout receipt drifted")
            require(budgets.get("hot_unallocated_bytes") == 0, f"{device.value}: hot budget accounting drifted")
            require(budgets.get("warm_budget_bytes") == warm_budget, f"{device.value}: warm profile budget drifted")
            require(budgets.get("warm_block_cache_bytes") == warm_block, f"{device.value}: warm block receipt drifted")
            require(budgets.get("warm_checkpoint_cache_bytes") == checkpoint, f"{device.value}: checkpoint receipt drifted")
            require(budgets.get("warm_source_window_cache_bytes") == source_window, f"{device.value}: source-window receipt drifted")
            require(budgets.get("warm_unallocated_bytes") == warm_remaining, f"{device.value}: warm unallocated receipt drifted")
            require(budgets.get("cold_budget_bytes") == cold_budget, f"{device.value}: cold profile budget drifted")
            require(budgets.get("cold_mapped_window_bytes") == cold_window, f"{device.value}: cold mapped receipt drifted")
            require(budgets.get("cold_unallocated_bytes") == cold_budget - cold_window, f"{device.value}: cold unallocated receipt drifted")
            require(budgets.get("within_profile_budgets") is True, f"{device.value}: policy exceeded profile cache budget")
            require(hot_block + warm_block <= MAX_BLOCK_CACHE_BYTES, f"{device.value}: block cache exceeds hard implementation cap")
    except (TestFailure, OSError, ValueError, json.JSONDecodeError, subprocess.SubprocessError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1

    print("Zevryon M8 profile runtime policy tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
