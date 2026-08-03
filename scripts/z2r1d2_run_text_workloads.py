#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
from typing import Any

from massivedoc_benchmark import run_measured


class WorkloadError(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise WorkloadError(message)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def binary(build_dir: Path, name: str) -> Path:
    candidate = build_dir / name
    if candidate.exists():
        return candidate
    windows = build_dir / "Release" / f"{name}.exe"
    if windows.exists():
        return windows
    raise WorkloadError(f"cannot find workload binary {name} in {build_dir}")


def result_of(measurement: dict[str, Any], label: str) -> dict[str, Any]:
    result = measurement.get("result")
    require(isinstance(result, dict), f"{label} did not emit a JSON object")
    return result


def validate_probe(measurement: dict[str, Any], mode: str) -> None:
    report = result_of(measurement, "probe")
    require(
        report.get("schema") == "zevryon.rust-shadow-text-probe.v1",
        "text probe schema mismatch",
    )
    require(report.get("fixture_bytes") == 65_536, "text probe fixture size changed")
    require(report.get("exact_verification") is True, "text probe exact verification failed")
    require(report.get("rust_shadow_mismatches") == 0, "text probe recorded a mismatch")
    stages = report.get("stages")
    require(isinstance(stages, list) and len(stages) == 4, "text probe stages are incomplete")
    require(
        [item.get("name") for item in stages if isinstance(item, dict)]
        == ["unicode", "grapheme", "script", "bidi"],
        "text probe stage order changed",
    )
    expected_enabled = mode == "shadow"
    require(
        report.get("rust_shadow_enabled") is expected_enabled,
        f"text probe Rust-shadow state does not match {mode}",
    )
    for stage in stages:
        require(isinstance(stage, dict), "text probe stage is not an object")
        require(stage.get("current_bytes") == 0, f"{stage.get('name')} leaked current bytes")
        require(stage.get("within_hard_limits") is True, f"{stage.get('name')} exceeded limits")
        require(stage.get("accounting_clean") is True, f"{stage.get('name')} accounting is dirty")
        require(stage.get("shadow_exact") is True, f"{stage.get('name')} exact verification failed")
        require(stage.get("shadow_mismatches") == 0, f"{stage.get('name')} mismatch recorded")
        require(
            stage.get("shadow_enabled") is expected_enabled,
            f"{stage.get('name')} Rust-shadow state changed",
        )
        if expected_enabled:
            require(stage.get("shadow_healthy") is True, f"{stage.get('name')} shadow unhealthy")
            require(int(stage.get("shadow_operations", 0)) > 0, f"{stage.get('name')} had no shadow operations")
            require(int(stage.get("shadow_verifications", 0)) > 0, f"{stage.get('name')} was not verified")


def validate_unicode(report: dict[str, Any]) -> None:
    require(report.get("schema") == "zevryon.unicode-benchmark.v1", "Unicode schema mismatch")
    require(report.get("fixture_bytes") == 65_536, "Unicode fixture size changed")
    require(report.get("iterations") == 1_024, "Unicode iteration count changed")
    require(float(report["p95_ms"]) <= 0.50, "Unicode P95 exceeded existing gate")
    require(float(report["p99_ms"]) <= 0.75, "Unicode P99 exceeded existing gate")
    require(report.get("rejected_reservations") == 0, "Unicode reservation rejected")
    require(report.get("accounting_errors") == 0, "Unicode accounting error")
    require(report.get("within_hard_limits") is True, "Unicode hard limit exceeded")
    require(report.get("accounting_clean") is True, "Unicode accounting dirty")


def validate_grapheme(report: dict[str, Any]) -> None:
    require(report.get("schema") == "zevryon.grapheme-benchmark.v1", "grapheme schema mismatch")
    require(report.get("unicode_version") == "17.0.0", "grapheme Unicode version changed")
    require(report.get("fixture_bytes") == 65_536, "grapheme fixture size changed")
    require(report.get("iterations") == 1_024, "grapheme iteration count changed")
    require(float(report["p95_ms"]) <= 1.50, "grapheme P95 exceeded existing gate")
    require(float(report["p99_ms"]) <= 2.00, "grapheme P99 exceeded existing gate")
    require(float(report["maximum_ms"]) <= 3.00, "grapheme maximum exceeded existing gate")
    require(report.get("rejected_reservations") == 0, "grapheme reservation rejected")
    require(report.get("accounting_errors") == 0, "grapheme accounting error")
    require(report.get("within_hard_limits") is True, "grapheme hard limit exceeded")
    require(report.get("accounting_clean") is True, "grapheme accounting dirty")


def validate_script(report: dict[str, Any]) -> None:
    require(report.get("schema") == "zevryon.script-run-benchmark.v1", "script schema mismatch")
    require(report.get("unicode_version") == "17.0.0", "script Unicode version changed")
    require(report.get("fixture_bytes") == 65_536, "script fixture size changed")
    require(report.get("iterations") == 1_024, "script iteration count changed")
    require(report.get("output_boundaries") == report.get("output_runs") + 1, "script boundary contract failed")
    require(report.get("rejected_reservations") == 0, "script reservation rejected")
    require(report.get("accounting_errors") == 0, "script accounting error")
    require(report.get("within_hard_limits") is True, "script hard limit exceeded")
    require(report.get("accounting_clean") is True, "script accounting dirty")


def validate_bidi(report: dict[str, Any]) -> None:
    require(report.get("schema") == "zevryon.bidi-explicit-benchmark.v1", "bidi schema mismatch")
    require(report.get("unicode_version") == "17.0.0", "bidi Unicode version changed")
    require(report.get("fixture_bytes") == 65_536, "bidi fixture size changed")
    require(report.get("iterations") == 1_024, "bidi iteration count changed")
    require(report.get("output_units") == report.get("input_codepoints"), "bidi unit contract failed")
    require(int(report.get("maximum_level", 126)) <= 125, "bidi maximum level invalid")
    require(report.get("rejected_reservations") == 0, "bidi reservation rejected")
    require(report.get("accounting_errors") == 0, "bidi accounting error")
    require(report.get("within_hard_limits") is True, "bidi hard limit exceeded")
    require(report.get("accounting_clean") is True, "bidi accounting dirty")


def validate_conformance(name: str, report: dict[str, Any]) -> None:
    expected_schema = {
        "grapheme": "zevryon.grapheme-conformance.v1",
        "script": "zevryon.script-conformance.v1",
        "bidi": "zevryon.bidi-conformance.v1",
    }[name]
    require(report.get("schema") == expected_schema, f"{name} conformance schema mismatch")
    require(report.get("unicode_version") == "17.0.0", f"{name} Unicode version changed")
    require(report.get("passed") is True, f"{name} conformance failed")
    if name == "grapheme":
        require(report.get("tests") == 766, "grapheme conformance test count changed")
    elif name == "script":
        require(report.get("codepoints") == 1_114_112, "script conformance coverage changed")
    else:
        require(report.get("codepoints") == 1_114_112, "bidi conformance coverage changed")


def main() -> int:
    parser = argparse.ArgumentParser(description="Run paired Unicode text production workloads")
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--ucd-dir", type=Path, required=True)
    parser.add_argument("--mode", choices=("baseline", "shadow"), required=True)
    parser.add_argument("--samples", type=int, default=3)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.samples < 3:
        parser.error("at least three benchmark samples are required")

    paths = {
        "probe": binary(args.build_dir, "zevryon-z2r1d2-text-probe"),
        "unicode": binary(args.build_dir, "zevryon-unicode-benchmark"),
        "grapheme_benchmark": binary(args.build_dir, "zevryon-grapheme-benchmark"),
        "grapheme_conformance": binary(args.build_dir, "zevryon-grapheme-conformance"),
        "script_benchmark": binary(args.build_dir, "zevryon-script-run-benchmark"),
        "script_conformance": binary(args.build_dir, "zevryon-script-conformance"),
        "bidi_benchmark": binary(args.build_dir, "zevryon-bidi-benchmark"),
        "bidi_conformance": binary(args.build_dir, "zevryon-bidi-conformance"),
    }

    probe = run_measured([str(paths["probe"])])
    validate_probe(probe, args.mode)

    commands = {
        "unicode": [str(paths["unicode"]), "1024", "4096", "2097152"],
        "grapheme": [str(paths["grapheme_benchmark"]), "1024", "1048576"],
        "script": [str(paths["script_benchmark"]), "1024", "262144"],
        "bidi": [str(paths["bidi_benchmark"]), "1024", "1048576"],
    }
    validators = {
        "unicode": validate_unicode,
        "grapheme": validate_grapheme,
        "script": validate_script,
        "bidi": validate_bidi,
    }
    benchmarks: dict[str, list[dict[str, Any]]] = {}
    for name, command in commands.items():
        samples = [run_measured(command) for _ in range(args.samples)]
        for sample in samples:
            validators[name](result_of(sample, f"{name} benchmark"))
        benchmarks[name] = samples

    conformance_commands = {
        "grapheme": [
            str(paths["grapheme_conformance"]),
            str(args.ucd_dir / "GraphemeBreakTest-17.0.0.txt"),
        ],
        "script": [
            str(paths["script_conformance"]),
            str(args.ucd_dir / "Scripts.txt"),
            str(args.ucd_dir / "ScriptExtensions.txt"),
        ],
        "bidi": [
            str(paths["bidi_conformance"]),
            str(args.ucd_dir / "DerivedBidiClass.txt"),
        ],
    }
    conformance: dict[str, dict[str, Any]] = {}
    for name, command in conformance_commands.items():
        measured = run_measured(command)
        validate_conformance(name, result_of(measured, f"{name} conformance"))
        conformance[name] = measured

    sources = {
        path.name: sha256_file(path)
        for path in sorted(args.ucd_dir.iterdir())
        if path.is_file()
    }
    report = {
        "schema": "zevryon.rust-shadow-text-workloads.v1",
        "mode": args.mode,
        "samples": args.samples,
        "ucd_sources_sha256": sources,
        "probe": probe,
        "benchmarks": benchmarks,
        "conformance": conformance,
    }
    text = json.dumps(report, indent=2, sort_keys=True) + "\n"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(text, encoding="utf-8")
    print(text, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
