#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys

SCRIPT_ROOT = Path(__file__).resolve().parent
if str(SCRIPT_ROOT) not in sys.path:
    sys.path.insert(0, str(SCRIPT_ROOT))

import m8_titan_fixture as titan


class TestFailure(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise TestFailure(message)


def run(command: list[str], expected: int) -> subprocess.CompletedProcess[str]:
    completed = subprocess.run(
        command,
        text=True,
        encoding="utf-8",
        errors="replace",
        capture_output=True,
        check=False,
        timeout=60.0,
    )
    require(
        completed.returncode == expected,
        f"exit mismatch: expected {expected}, got {completed.returncode}; stdout={completed.stdout!r}; stderr={completed.stderr!r}",
    )
    return completed


def run_smoke(root: Path, name: str) -> dict[str, object]:
    corpus = root / f"{name}.zmdoc"
    report = root / f"{name}.json"
    run(
        [
            sys.executable,
            str(SCRIPT_ROOT / "m8_titan_fixture.py"),
            "--output",
            str(corpus),
            "--report",
            str(report),
        ],
        0,
    )
    require(corpus.is_file(), "Titan smoke corpus was not created")
    require(report.is_file(), "Titan smoke report was not created")
    value = json.loads(report.read_text(encoding="utf-8"))
    require(value.get("schema") == titan.SCHEMA, "Titan report schema drifted")
    require(value.get("authority") == titan.AUTHORITY, "Titan report authority drifted")
    require(value.get("mode") == "smoke", "Titan smoke mode drifted")
    require(value.get("gate_passed") is True, "Titan smoke gate failed")
    require(value.get("certification_threshold_met") is False, "smoke reached certification threshold")
    require(value.get("certification_eligible") is False, "smoke became certification evidence")
    require(value.get("observed_envelope") == titan.smoke_config().as_dict(), "smoke envelope drifted")
    require(
        value.get("frozen_certification_envelope") == titan.certification_config().as_dict(),
        "frozen Titan certification envelope drifted",
    )
    generation = value.get("generation")
    verification = value.get("verification")
    require(isinstance(generation, dict), "generation receipt missing")
    require(isinstance(verification, dict), "verification receipt missing")
    require(generation.get("physical_bytes") == titan.expected_physical_bytes(titan.smoke_config()), "physical byte receipt drifted")
    require(isinstance(generation.get("container_sha256"), str) and len(generation["container_sha256"]) == 64, "container hash missing")
    require(isinstance(generation.get("payload_sha256"), str) and len(generation["payload_sha256"]) == 64, "payload hash missing")
    giant = verification.get("giant_record")
    token = verification.get("unbroken_token")
    grapheme = verification.get("pathological_grapheme")
    require(isinstance(giant, dict) and giant.get("payload_bytes") == titan.SMOKE_GIANT_RECORD_BYTES, "giant record receipt drifted")
    require(giant.get("giant_record_is_not_a_giant_token") is True, "giant record accidentally became token evidence")
    require(isinstance(token, dict) and token.get("payload_bytes") == titan.SMOKE_UNBROKEN_TOKEN_BYTES, "token receipt drifted")
    require(token.get("contains_only_token_byte") is True, "token payload proof failed")
    require(isinstance(grapheme, dict) and grapheme.get("payload_bytes") == titan.SMOKE_PATHOLOGICAL_GRAPHEME_BYTES, "grapheme receipt drifted")
    require(grapheme.get("single_extended_grapheme_construction") is True, "grapheme construction proof failed")
    with corpus.open("rb") as handle:
        handle.seek(int(token["payload_offset"]))
        require(handle.read(32) == b"T" * 32, "raw token bytes are not contiguous ASCII token bytes")
        handle.seek(int(grapheme["payload_offset"]))
        expected = titan.pathological_grapheme_payload(titan.SMOKE_PATHOLOGICAL_GRAPHEME_BYTES)
        require(handle.read(len(expected)) == expected, "raw pathological grapheme bytes drifted")
    return value


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--work-dir", type=Path, required=True)
    args = parser.parse_args()
    root = args.work_dir.resolve()
    try:
        if root.exists():
            shutil.rmtree(root)
        root.mkdir(parents=True)

        cert = titan.certification_config()
        require(cert.logical_bytes == 4 * 1024 * 1024 * 1024, "Titan 4 GiB threshold drifted")
        require(cert.records == 8_388_608, "Titan record threshold drifted")
        require(cert.logical_nodes == 67_108_864, "Titan node threshold drifted")
        require(cert.style_runs == 33_554_432, "Titan style threshold drifted")
        require(cert.resource_references == 1_048_576, "Titan resource threshold drifted")
        require(cert.giant_record_bytes == 64 * 1024 * 1024, "Titan giant-record threshold drifted")
        require(cert.unbroken_token_bytes == 16 * 1024 * 1024, "Titan token threshold drifted")
        require(cert.pathological_grapheme_bytes == 64 * 1024, "Titan grapheme threshold drifted")
        titan.validate_config(cert, certification=True)

        first = run_smoke(root, "first")
        second = run_smoke(root, "second")
        require(first["generation"]["container_sha256"] == second["generation"]["container_sha256"], "same-config container replay hash drifted")
        require(first["generation"]["payload_sha256"] == second["generation"]["payload_sha256"], "same-config payload replay hash drifted")
        require(first["verification"]["giant_record"]["sha256"] == second["verification"]["giant_record"]["sha256"], "giant-record replay hash drifted")
        require(first["verification"]["unbroken_token"]["sha256"] == second["verification"]["unbroken_token"]["sha256"], "token replay hash drifted")
        require(first["verification"]["pathological_grapheme"]["sha256"] == second["verification"]["pathological_grapheme"]["sha256"], "grapheme replay hash drifted")

        invalid_corpus = root / "invalid-cert.zmdoc"
        invalid_report = root / "invalid-cert.json"
        run(
            [
                sys.executable,
                str(SCRIPT_ROOT / "m8_titan_fixture.py"),
                "--certification",
                "--logical-bytes",
                str(titan.SMOKE_LOGICAL_BYTES),
                "--output",
                str(invalid_corpus),
                "--report",
                str(invalid_report),
            ],
            1,
        )
        require(not invalid_corpus.exists(), "invalid certification request created a corpus")
        require(not invalid_report.exists(), "invalid certification request created a report")
    except (TestFailure, OSError, ValueError, json.JSONDecodeError, subprocess.SubprocessError, titan.TitanEvidenceInvalid) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1
    finally:
        shutil.rmtree(root, ignore_errors=True)

    print("Zevryon M8 canonical Titan fixture tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
