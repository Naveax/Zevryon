#!/usr/bin/env python3
from __future__ import annotations

import importlib.util
from pathlib import Path
import subprocess
import sys
from unittest import mock

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT))

from zevryon_platform.competitor_adapter import AdapterRequest  # noqa: E402
from zevryon_platform.competitor_lab import Engine, LabSystemState  # noqa: E402
from zevryon_platform.competitor_lab_v2 import canonical_workload_sha256  # noqa: E402

MODULE_PATH = REPO_ROOT / "scripts" / "m7_zevryon_adapter.py"
SPEC = importlib.util.spec_from_file_location("m7_zevryon_adapter", MODULE_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError("cannot load M7 Zevryon adapter module")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)

SHA = "ab" * 32


def require(value: bool, message: str) -> None:
    if not value:
        raise AssertionError(message)


def workload() -> dict[str, object]:
    return {
        "schema": "zevryon.m7.workload.v1",
        "corpus_sha256": SHA,
        "corpus_logical_bytes": 1024 * 1024,
        "viewport": {
            "width_px": 1440,
            "height_px": 900,
            "overscan_px": 720,
            "max_fragments": 512,
        },
        "operations": [
            {"kind": "open_preindexed"},
            {"kind": "open_streaming"},
            {"kind": "scroll", "samples": 1000, "warmup": 2, "step_px": 18},
            {"kind": "exact_search_warm", "query_utf8": "needle", "trials": 5},
            {
                "kind": "exact_search_cold",
                "query_utf8": "needle",
                "trials": 5,
                "fresh_process_each_trial": True,
            },
            {"kind": "mutation_batch", "count": 100},
            {"kind": "copy_all", "trials": 5},
        ],
    }


def request() -> AdapterRequest:
    value = workload()
    return AdapterRequest(
        engine=Engine.ZEVRYON,
        run_index=1,
        workload_sha256=canonical_workload_sha256(value),
        corpus_sha256=SHA,
        corpus_logical_bytes=1024 * 1024,
        corpus_path="fixture.zmdoc",
        workload=value,
        system_state=LabSystemState(
            os_name="Linux",
            os_release="fixture",
            architecture="x86_64",
            cpu_model="fixture CPU",
            physical_ram_mib=16384,
            thermal_state="nominal",
            power_mode="performance",
        ),
    )


def fake_run(command, **kwargs):
    require(command[2] == "desktop", "campaign RAM did not select desktop")
    sample_path = Path(command[3])
    values = ["1.0"] * 990 + ["2.0"] * 10
    sample_path.write_text("\n".join(values) + "\n", encoding="ascii")
    envelope = (
        '{"operation":"zenith-tab-runtime-frame-probe",'
        '"profile":"desktop","frame_budget_us":8330,'
        '"warmup_samples":2,"recorded_samples":1000,'
        '"visible_layouts":1002,"frame_overruns":0,'
        '"prefetch_accepts":0,"pool_thread_starts":2,'
        '"pool_ready_peak_bytes":0}\n'
    )
    return subprocess.CompletedProcess(command, 0, envelope, "")


def main() -> int:
    req = request()
    with mock.patch.object(MODULE.subprocess, "run", side_effect=fake_run):
        metrics = MODULE.measure_frame_metrics(
            Path("fake-frame-probe"), Path("fake-store"), req, 5.0
        )
    require(metrics["scroll_p99_ms"] == 1.0, "nearest-rank P99 changed")
    require(metrics["maximum_normal_stall_ms"] == 2.0, "maximum stall changed")
    require(MODULE.profile_for_campaign_ram(2048).value == "legacy-phone", "legacy profile")
    require(MODULE.profile_for_campaign_ram(4096).value == "mid-phone", "mid profile")
    require(MODULE.profile_for_campaign_ram(8192).value == "modern-phone", "modern profile")
    require(MODULE.profile_for_campaign_ram(16384).value == "desktop", "desktop profile")
    missing = sorted(set(MODULE.CORE_METRIC_NAMES) - MODULE.FRAME_METRICS)
    require(len(missing) == 7, "readiness adapter unexpectedly certifies extra metrics")
    require("mutation_p95_us" in missing, "mutation readiness debt disappeared")
    require("exact_search_cold_ms" in missing, "cold-search readiness debt disappeared")
    print("Zevryon M7 readiness adapter smoke passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
