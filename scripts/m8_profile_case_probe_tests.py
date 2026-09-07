#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
TITAN_SCRIPT = ROOT / "scripts" / "m8_titan_fixture.py"


class TestFailure(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise TestFailure(message)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def run(command: list[str], *, timeout: float = 180.0) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        text=True,
        encoding="utf-8",
        errors="strict",
        capture_output=True,
        check=False,
        timeout=timeout,
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    args = parser.parse_args()

    try:
        probe = args.probe.resolve()
        require(probe.is_file(), f"profile case probe not found: {probe}")
        work = args.work_dir.resolve()
        if work.exists():
            shutil.rmtree(work)
        work.mkdir(parents=True)

        titan = work / "titan.zmdoc"
        titan_report = work / "titan-report.json"
        generated = run(
            [
                sys.executable,
                str(TITAN_SCRIPT),
                "--output",
                str(titan),
                "--report",
                str(titan_report),
            ]
        )
        require(
            generated.returncode == 0,
            f"Titan smoke generation failed: stdout={generated.stdout!r}; stderr={generated.stderr!r}",
        )
        titan_document = json.loads(titan_report.read_text(encoding="utf-8"))
        require(titan_document["mode"] == "smoke", "profile probe smoke did not use smoke Titan")
        require(titan_document["certification_eligible"] is False, "smoke Titan became certification eligible")

        probe_work = work / "probe-work"
        completed = run([str(probe), "legacy-phone", str(titan), str(probe_work)])
        require(
            completed.returncode == 0,
            f"profile case probe failed: stdout={completed.stdout!r}; stderr={completed.stderr!r}",
        )
        document = json.loads(completed.stdout)
        require(document.get("schema") == "zevryon.m8.profile-probe.v1", "profile probe schema drifted")
        require(document.get("device_class") == "legacy-phone", "profile probe class drifted")
        policy = document.get("runtime_policy")
        require(isinstance(policy, dict) and policy.get("within_profile_budgets") is True, "runtime policy receipt invalid")
        require(policy.get("layout_store_read_policy_applied") is True, "layout engine did not apply the profile StoreReadConfig")

        store = document.get("store")
        require(isinstance(store, dict), "store receipt missing")
        envelope = titan_document["observed_envelope"]
        require(store.get("logical_utf8_bytes") == envelope["logical_utf8_bytes"], "store logical bytes differ from Titan")
        require(store.get("logical_records") == envelope["logical_records"], "store record count differs from Titan")
        require(store.get("payload_sha256") == titan_document["generation"]["payload_sha256"], "store payload SHA differs from Titan")

        streaming = document.get("streaming")
        require(isinstance(streaming, dict), "streaming receipt missing")
        require(streaming.get("preview_records") == 1, "streaming preview threshold drifted")
        require(streaming.get("remaining_records", 0) > 0, "streaming preview happened after completion")
        require(streaming.get("fragment_count", 0) > 0, "streaming viewport had no fragments")
        require(streaming.get("truncated") is False, "streaming viewport truncated")

        preindexed = document.get("preindexed")
        require(isinstance(preindexed, dict) and preindexed.get("fragment_count", 0) > 0, "preindexed viewport receipt invalid")
        require(preindexed.get("truncated") is False, "preindexed viewport truncated")

        scroll = document.get("scroll")
        require(isinstance(scroll, dict), "scroll receipt missing")
        require(scroll.get("warmup_count") == 16, "scroll warmup count drifted")
        require(scroll.get("measured_count") == 257, "scroll measured count drifted")
        require(len(scroll.get("coordinates_q8", [])) == 257, "scroll coordinate count drifted")
        require(len(scroll.get("samples_ms", [])) == 257, "scroll sample count drifted")
        require(all(value >= 0 for value in scroll["samples_ms"]), "negative scroll timing")

        search = document.get("search")
        require(isinstance(search, dict), "search receipt missing")
        require(search.get("query") == "ZEVRYON_M8_TITAN_TAIL", "search query drifted")
        require(search.get("terminal_record_index") + 1 == envelope["logical_records"], "tail marker record drifted")
        require(search.get("terminal_logical_id") + 1 == envelope["logical_records"], "tail marker logical id drifted")

        mutation = document.get("mutation")
        require(isinstance(mutation, dict), "mutation receipt missing")
        require(mutation.get("warmup_count") == 16, "mutation warmup count drifted")
        require(mutation.get("measured_count") == 257, "mutation measured count drifted")
        require(len(mutation.get("indices", [])) == 257, "mutation index count drifted")
        require(len(mutation.get("samples_us", [])) == 257, "mutation sample count drifted")
        require(mutation.get("restored") is True, "mutation state was not restored")

        copy_receipt = document.get("copy")
        require(isinstance(copy_receipt, dict), "copy receipt missing")
        require(copy_receipt.get("source_bytes") == envelope["logical_utf8_bytes"], "copy source bytes drifted")
        require(copy_receipt.get("output_bytes") == envelope["logical_utf8_bytes"], "copy output bytes drifted")
        require(copy_receipt.get("cancelled") is False, "copy unexpectedly cancelled")
        copy_output = probe_work / "copy-output.txt"
        require(copy_output.is_file(), "copy output file missing")
        require(sha256_file(copy_output) == titan_document["generation"]["payload_sha256"], "copy output SHA differs from Titan payload")
        copy_output.read_text(encoding="utf-8", errors="strict")

        repeated = run([str(probe), "legacy-phone", str(titan), str(probe_work)], timeout=30.0)
        require(repeated.returncode != 0, "profile probe overwrote an existing attempt directory")
    except (TestFailure, OSError, ValueError, json.JSONDecodeError, subprocess.SubprocessError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1

    print("m8-profile-case-probe-tests: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
