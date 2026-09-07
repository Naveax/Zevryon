#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
from typing import Any

SCHEMA = "zevryon.m8.storage-process-crash.v1"
INJECTED_CRASH_EXIT_CODE = 86
PUBLICATION_CUTS = (
    "after-payload-flush",
    "after-prepare",
    "after-manifest-temp",
    "after-manifest",
    "after-commit",
)
COMPACTION_CUTS = (
    "after-journal-temp",
    "after-journal-replace",
    "after-stale-quarantine",
)


class CrashGateInvalid(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise CrashGateInvalid(message)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        while True:
            chunk = handle.read(1024 * 1024)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def expected_identity_hex(generation: int) -> str:
    return "".join(f"{(generation * 17 + index) & 0xff:02x}" for index in range(32))


def run_probe(
    probe: Path,
    *args: str,
    expected_exit: int = 0,
    timeout: float = 30.0,
) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        [str(probe), *args],
        text=True,
        encoding="utf-8",
        errors="replace",
        capture_output=True,
        timeout=timeout,
        check=False,
    )
    if result.returncode != expected_exit:
        raise CrashGateInvalid(
            "probe exit mismatch for "
            + " ".join(args)
            + f": expected {expected_exit}, got {result.returncode}; "
            + f"stdout={result.stdout!r}; stderr={result.stderr!r}"
        )
    return result


def recover(probe: Path, root: Path) -> dict[str, Any]:
    result = run_probe(probe, "recover", str(root))
    try:
        value = json.loads(result.stdout)
    except json.JSONDecodeError as exc:
        raise CrashGateInvalid(f"recovery probe emitted invalid JSON: {result.stdout!r}") from exc
    if not isinstance(value, dict):
        raise CrashGateInvalid("recovery probe JSON is not an object")
    return value


def validate_generation(value: dict[str, Any], generation: int, context: str) -> None:
    require(value.get("protocol_present") is True, f"{context}: protocol disappeared")
    require(value.get("found") is True, f"{context}: committed authority missing")
    require(value.get("generation") == generation, f"{context}: wrong generation")
    require(
        value.get("identity_hex") == expected_identity_hex(generation),
        f"{context}: source identity drifted",
    )
    require(value.get("authority_bytes") == 160, f"{context}: authority size drifted")
    require(value.get("authority_first") == (generation & 0xff), f"{context}: authority payload drifted")
    require(
        value.get("segments") == [{"id": 0, "bytes": 7}],
        f"{context}: segment inventory drifted",
    )


def count_suffix(root: Path, suffix: str) -> int:
    if not root.exists():
        return 0
    return sum(
        1
        for path in root.rglob("*")
        if path.is_file() and suffix in path.name
    )


def publication_case(probe: Path, base: Path, cut: str) -> dict[str, Any]:
    root = base / f"publication-{cut}"
    run_probe(probe, "seed", str(root), "1")
    crashed = run_probe(
        probe,
        "crash-publish",
        str(root),
        "2",
        cut,
        expected_exit=INJECTED_CRASH_EXIT_CODE,
    )
    after_crash = recover(probe, root)
    expected_after_crash = 2 if cut == "after-commit" else 1
    validate_generation(after_crash, expected_after_crash, f"publication/{cut}/after-crash")

    record: dict[str, Any] = {
        "cut": cut,
        "injected_exit_code": crashed.returncode,
        "recovery_after_crash": after_crash,
        "uncommitted_quarantine_before_retry": count_suffix(root, ".uncommitted"),
    }

    if cut != "after-commit":
        run_probe(probe, "publish", str(root), "2", "none")
        after_retry = recover(probe, root)
        validate_generation(after_retry, 2, f"publication/{cut}/after-retry")
        record["recovery_after_retry"] = after_retry
        record["uncommitted_quarantine_after_retry"] = count_suffix(root, ".uncommitted")
        if cut == "after-manifest":
            require(
                record["uncommitted_quarantine_after_retry"] == 1,
                "published-uncommitted manifest was not preserved exactly once during retry",
            )
        else:
            require(
                record["uncommitted_quarantine_after_retry"] == 0,
                f"unexpected uncommitted quarantine artifact for {cut}",
            )
    else:
        require(
            count_suffix(root, ".uncommitted") == 0,
            "committed generation was mislabeled as uncommitted quarantine evidence",
        )

    record["gate_passed"] = True
    return record


def compaction_case(probe: Path, base: Path, cut: str) -> dict[str, Any]:
    root = base / f"compaction-{cut}"
    run_probe(probe, "seed", str(root), "4")
    crashed = run_probe(
        probe,
        "crash-compact",
        str(root),
        cut,
        expected_exit=INJECTED_CRASH_EXIT_CODE,
    )
    after_crash = recover(probe, root)
    validate_generation(after_crash, 4, f"compaction/{cut}/after-crash")

    stale_before_finish = count_suffix(root, ".stale")
    expected_stale_before = 1 if cut == "after-stale-quarantine" else 0
    require(
        stale_before_finish == expected_stale_before,
        f"compaction/{cut}: stale quarantine count before resume drifted",
    )

    run_probe(probe, "compact", str(root), "none")
    after_resume = recover(probe, root)
    validate_generation(after_resume, 4, f"compaction/{cut}/after-resume")
    stale_after_finish = count_suffix(root, ".stale")
    require(
        stale_after_finish == 2,
        f"compaction/{cut}: resumed compaction did not quarantine exactly two stale manifests",
    )

    return {
        "cut": cut,
        "injected_exit_code": crashed.returncode,
        "recovery_after_crash": after_crash,
        "stale_quarantine_before_resume": stale_before_finish,
        "recovery_after_resume": after_resume,
        "stale_quarantine_after_resume": stale_after_finish,
        "gate_passed": True,
    }


def build_report(probe: Path, work_dir: Path) -> dict[str, Any]:
    if not probe.is_file():
        raise CrashGateInvalid(f"crash probe does not exist: {probe}")
    if work_dir.exists():
        shutil.rmtree(work_dir)
    work_dir.mkdir(parents=True)

    publication = [publication_case(probe, work_dir, cut) for cut in PUBLICATION_CUTS]
    compaction = [compaction_case(probe, work_dir, cut) for cut in COMPACTION_CUTS]
    return {
        "schema": SCHEMA,
        "authority": "m8-fresh-process-storage-crash-cut-recovery-v1",
        "probe_sha256": sha256_file(probe),
        "injected_crash_exit_code": INJECTED_CRASH_EXIT_CODE,
        "publication_cuts": list(PUBLICATION_CUTS),
        "compaction_cuts": list(COMPACTION_CUTS),
        "publication_results": publication,
        "compaction_results": compaction,
        "fresh_process_recovery": True,
        "power_loss_certified": False,
        "gate_passed": True,
    }


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Inject abrupt child-process exits at every frozen M8 storage transaction cut and verify recovery in fresh processes."
    )
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    report: dict[str, Any]
    try:
        report = build_report(args.probe.resolve(), args.work_dir.resolve())
    except (CrashGateInvalid, OSError, subprocess.SubprocessError) as exc:
        report = {
            "schema": SCHEMA,
            "authority": "m8-fresh-process-storage-crash-cut-recovery-v1",
            "gate_passed": False,
            "error": str(exc),
        }
        text = json.dumps(report, indent=2, sort_keys=True) + "\n"
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text, encoding="utf-8")
        print(text, end="", file=sys.stderr)
        return 1

    text = json.dumps(report, indent=2, sort_keys=True) + "\n"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(text, encoding="utf-8")
    print(text, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
