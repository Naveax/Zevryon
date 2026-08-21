#!/usr/bin/env python3
from __future__ import annotations

from pathlib import Path
import sys

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT))

from zevryon_platform.competitor_adapter import (  # noqa: E402
    AdapterRequest,
    PROTOCOL,
    invoke_adapter,
)
from zevryon_platform.competitor_lab import CORE_METRIC_NAMES, Engine, LabSystemState  # noqa: E402
from zevryon_platform.competitor_lab_v2 import canonical_workload_sha256  # noqa: E402

CORPUS_SHA = "ab" * 32
WORKLOAD = {
    "schema": "zevryon.m7.workload.v1",
    "corpus_sha256": CORPUS_SHA,
    "corpus_logical_bytes": 4096,
    "viewport": {"width": 1440, "height": 900},
    "operations": [{"kind": "scroll", "distance_css_px": 1000}],
}
WORKLOAD_SHA = canonical_workload_sha256(WORKLOAD)
STATE = LabSystemState(
    os_name="Linux",
    os_release="fixture",
    architecture="x86_64",
    cpu_model="fixture CPU",
    physical_ram_mib=16384,
    thermal_state="nominal",
    power_mode="performance",
)


def require(value: bool, message: str) -> None:
    if not value:
        raise AssertionError(message)


def request() -> AdapterRequest:
    return AdapterRequest(
        engine=Engine.CHROME,
        run_index=3,
        workload_sha256=WORKLOAD_SHA,
        corpus_sha256=CORPUS_SHA,
        corpus_logical_bytes=4096,
        corpus_path="fixture.zmdoc",
        workload=WORKLOAD,
        system_state=STATE,
    )


def adapter_code(mode: str) -> str:
    metrics_literal = repr(
        {name: float(index + 1) for index, name in enumerate(CORE_METRIC_NAMES)}
    )
    return f"""
import json, sys, time
request = json.loads(sys.stdin.read())
mode = {mode!r}
if mode == 'timeout':
    time.sleep(1.0)
if mode == 'nonzero':
    print('fixture adapter failure', file=sys.stderr)
    raise SystemExit(7)
response = {{
    'protocol': {PROTOCOL!r},
    'engine': request['engine'],
    'engine_version': 'fixture-browser-1',
    'workload_sha256': request['workload_sha256'],
    'corpus_sha256': request['corpus_sha256'],
    'corpus_logical_bytes': request['corpus_logical_bytes'],
    'captured_at_utc': '2026-08-21T18:00:00Z',
    'run_index': request['run_index'],
    'system_state': request['system_state'],
    'metrics': {metrics_literal},
    'failure_mode': None,
}}
if mode == 'wrong-workload':
    response['workload_sha256'] = 'cd' * 32
if mode == 'extra-output':
    print('diagnostic that corrupts stdout')
print(json.dumps(response, sort_keys=True))
"""


def run(mode: str, timeout: float = 5.0):
    return invoke_adapter(
        [sys.executable, "-c", adapter_code(mode)],
        request(),
        timeout,
    )


def main() -> int:
    success = run("success")
    require(success.run.succeeded, "valid adapter response failed")
    require(success.run.engine == Engine.CHROME, "engine identity changed")
    require(success.workload_sha256 == WORKLOAD_SHA, "workload identity changed")
    require(success.run.metrics["scroll_p99_ms"] > 0.0, "metrics not preserved")

    wrong = run("wrong-workload")
    require(not wrong.run.succeeded, "wrong workload identity was accepted")
    require(
        "workload identity mismatch" in str(wrong.run.failure_mode),
        "wrong-workload diagnostic",
    )

    noisy = run("extra-output")
    require(not noisy.run.succeeded, "extra stdout was accepted")
    require("protocol failure" in str(noisy.run.failure_mode), "extra-output diagnostic")

    nonzero = run("nonzero")
    require(not nonzero.run.succeeded, "nonzero adapter exit was accepted")
    require("exited 7" in str(nonzero.run.failure_mode), "nonzero diagnostic")

    timed = run("timeout", timeout=0.05)
    require(not timed.run.succeeded, "adapter timeout was accepted")
    require("timeout" in str(timed.run.failure_mode), "timeout diagnostic")

    print("Zevryon M7 competitor adapter protocol smoke passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
