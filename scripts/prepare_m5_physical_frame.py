#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
from typing import Any, Sequence

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_GIANT_BYTES = 64 * 1024 * 1024
DEFAULT_SAMPLES = 2000
DEFAULT_WARMUP = 120
DEFAULT_WIDTH_PX = 1440
DEFAULT_HEIGHT_PX = 900
DEFAULT_OVERSCAN_PX = 720
DEFAULT_MAX_FRAGMENTS = 512
DEFAULT_STEP_PX = 18
DEFAULT_STRIDE_KIB = 16


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Build Zevryon Release binaries, prepare a deterministic giant-record "
            "MassiveDoc workload, and run the physical native-frame certification gate."
        )
    )
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build-m5-physical")
    parser.add_argument("--work-dir", type=Path, default=ROOT / ".m5-physical-frame")
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--giant-record-bytes", type=int, default=DEFAULT_GIANT_BYTES)
    parser.add_argument("--segment-mib", type=int, default=16)
    parser.add_argument("--samples", type=int, default=DEFAULT_SAMPLES)
    parser.add_argument("--warmup", type=int, default=DEFAULT_WARMUP)
    parser.add_argument("--width-px", type=int, default=DEFAULT_WIDTH_PX)
    parser.add_argument("--height-px", type=int, default=DEFAULT_HEIGHT_PX)
    parser.add_argument("--overscan-px", type=int, default=DEFAULT_OVERSCAN_PX)
    parser.add_argument("--max-fragments", type=int, default=DEFAULT_MAX_FRAGMENTS)
    parser.add_argument("--step-px", type=int, default=DEFAULT_STEP_PX)
    parser.add_argument("--timeout-seconds", type=int, default=900)
    parser.add_argument("--keep-work-dir", action="store_true")
    return parser.parse_args()


def _positive(name: str, value: int, *, allow_zero: bool = False) -> int:
    minimum = 0 if allow_zero else 1
    if value < minimum:
        raise ValueError(f"{name} must be >= {minimum}")
    return value


def _run(
    command: Sequence[str],
    *,
    timeout: int,
    cwd: Path = ROOT,
    require_success: bool = True,
) -> subprocess.CompletedProcess[str]:
    completed = subprocess.run(
        list(command),
        cwd=cwd,
        capture_output=True,
        text=True,
        check=False,
        timeout=timeout,
    )
    if require_success and completed.returncode != 0:
        stdout = completed.stdout.rstrip() or "<empty>"
        stderr = completed.stderr.rstrip() or "<empty>"
        raise RuntimeError(
            f"command failed ({completed.returncode}): {' '.join(command)}\n"
            f"--- stdout ---\n{stdout}\n"
            f"--- stderr ---\n{stderr}"
        )
    return completed


def _last_json(stdout: str) -> dict[str, Any]:
    text = stdout.strip()
    if not text:
        raise ValueError("command produced no JSON output")
    try:
        parsed = json.loads(text)
        if not isinstance(parsed, dict):
            raise ValueError("command JSON output is not an object")
        return parsed
    except json.JSONDecodeError:
        lines = [line.strip() for line in stdout.splitlines() if line.strip()]
        for line in reversed(lines):
            try:
                parsed = json.loads(line)
            except json.JSONDecodeError:
                continue
            if isinstance(parsed, dict):
                return parsed
    raise ValueError("command produced no JSON object")


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while True:
            chunk = stream.read(1024 * 1024)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def _safe_reset_work_dir(path: Path) -> None:
    resolved = path.resolve()
    forbidden = {ROOT.resolve(), ROOT.parent.resolve(), Path(resolved.anchor)}
    if resolved in forbidden:
        raise ValueError(f"refusing to clear unsafe work directory: {resolved}")
    if path.exists():
        shutil.rmtree(path)
    path.mkdir(parents=True, exist_ok=True)


def _resolve_executable(build_dir: Path, stem: str) -> Path:
    candidates = [
        build_dir / "Release" / f"{stem}.exe",
        build_dir / f"{stem}.exe",
        build_dir / "Release" / stem,
        build_dir / stem,
    ]
    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve()
    rendered = ", ".join(str(path) for path in candidates)
    raise FileNotFoundError(f"unable to locate {stem}; checked: {rendered}")


def _validate_environment() -> None:
    if os.environ.get("ZEVRYON_PHYSICAL_DEVICE") != "1":
        raise ValueError(
            "physical certification requires ZEVRYON_PHYSICAL_DEVICE=1"
        )
    thermal_state = os.environ.get("ZEVRYON_THERMAL_STATE", "").strip().lower()
    if thermal_state not in {"nominal", "fair"}:
        raise ValueError(
            "physical certification requires "
            "ZEVRYON_THERMAL_STATE=nominal or fair"
        )


def _configure_and_build(args: argparse.Namespace) -> tuple[Path, Path]:
    build_dir = args.build_dir.resolve()
    if not args.skip_build:
        _run(
            [
                args.cmake,
                "-S",
                str(ROOT),
                "-B",
                str(build_dir),
                "-DCMAKE_BUILD_TYPE=Release",
                "-DBUILD_TESTING=OFF",
            ],
            timeout=args.timeout_seconds,
        )
        _run(
            [
                args.cmake,
                "--build",
                str(build_dir),
                "--config",
                "Release",
                "--target",
                "zevryon-massivedoc",
                "zevryon-zenith-frame-probe",
            ],
            timeout=args.timeout_seconds,
        )
    return (
        _resolve_executable(build_dir, "zevryon-massivedoc"),
        _resolve_executable(build_dir, "zevryon-zenith-frame-probe"),
    )


def main() -> int:
    args = parse_args()
    try:
        _positive("giant-record-bytes", args.giant_record_bytes)
        _positive("segment-mib", args.segment_mib)
        _positive("samples", args.samples)
        _positive("warmup", args.warmup, allow_zero=True)
        _positive("width-px", args.width_px)
        _positive("height-px", args.height_px)
        _positive("overscan-px", args.overscan_px, allow_zero=True)
        _positive("max-fragments", args.max_fragments)
        _positive("step-px", args.step_px)
        _positive("timeout-seconds", args.timeout_seconds)
        if args.samples < 1000:
            raise ValueError("physical certification requires at least 1000 samples")
        _validate_environment()
        massivedoc, probe = _configure_and_build(args)
        _safe_reset_work_dir(args.work_dir)
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as exc:
        print(f"physical frame preparation failed: {exc}", file=sys.stderr)
        return 1

    work_dir = args.work_dir.resolve()
    corpus = work_dir / "corpus.zmdoc"
    store = work_dir / "store"
    samples = work_dir / "frame.samples.txt"
    evidence = work_dir / "frame-certification.json"
    manifest_path = work_dir / "manifest.json"
    logical_bytes = args.giant_record_bytes + 1

    generator = ROOT / "scripts" / "generate_massivedoc_corpus.py"
    certifier = ROOT / "scripts" / "certify_native_frame_latency.py"

    commands: list[dict[str, Any]] = []
    try:
        generated = _run(
            [
                sys.executable,
                str(generator),
                str(corpus),
                "--logical-bytes",
                str(logical_bytes),
                "--records",
                "2",
                "--largest-record-limit-bytes",
                str(args.giant_record_bytes),
                "--giant-record-bytes",
                str(args.giant_record_bytes),
                "--giant-record-index",
                "0",
            ],
            timeout=args.timeout_seconds,
        )
        generation_summary = _last_json(generated.stdout)
        commands.append({"stage": "generate", "returncode": generated.returncode})

        imported = _run(
            [
                str(massivedoc),
                "import",
                str(corpus),
                str(store),
                str(args.segment_mib),
            ],
            timeout=args.timeout_seconds,
        )
        import_summary = _last_json(imported.stdout)
        commands.append({"stage": "import", "returncode": imported.returncode})

        verified = _run(
            [str(massivedoc), "verify", str(store)],
            timeout=args.timeout_seconds,
        )
        verify_summary = _last_json(verified.stdout)
        commands.append({"stage": "verify", "returncode": verified.returncode})

        arena = _run(
            [str(massivedoc), "arena-build", str(store), "96", "18"],
            timeout=args.timeout_seconds,
        )
        arena_summary = _last_json(arena.stdout)
        commands.append({"stage": "arena-build", "returncode": arena.returncode})

        checkpoint = _run(
            [
                str(massivedoc),
                "checkpoint-build",
                str(store),
                "0",
                str(args.width_px),
                str(DEFAULT_STRIDE_KIB),
            ],
            timeout=args.timeout_seconds,
        )
        checkpoint_summary = _last_json(checkpoint.stdout)
        commands.append({"stage": "checkpoint-build", "returncode": checkpoint.returncode})

        certification_command = [
            sys.executable,
            str(certifier),
            "--probe",
            str(probe),
            "--store",
            str(store),
            "--output",
            str(evidence),
            "--samples-output",
            str(samples),
            "--samples",
            str(args.samples),
            "--warmup",
            str(args.warmup),
            "--width-px",
            str(args.width_px),
            "--height-px",
            str(args.height_px),
            "--overscan-px",
            str(args.overscan_px),
            "--max-fragments",
            str(args.max_fragments),
            "--step-px",
            str(args.step_px),
            "--timeout-seconds",
            str(args.timeout_seconds),
            "--require-pass",
        ]
        certified = _run(
            certification_command,
            timeout=args.timeout_seconds,
            require_success=False,
        )
        commands.append(
            {"stage": "certify", "returncode": certified.returncode}
        )

        if not evidence.is_file():
            stdout = certified.stdout.rstrip() or "<empty>"
            stderr = certified.stderr.rstrip() or "<empty>"
            raise RuntimeError(
                "certifier did not produce evidence artifact\n"
                f"--- stdout ---\n{stdout}\n"
                f"--- stderr ---\n{stderr}"
            )
        certification = json.loads(evidence.read_text(encoding="utf-8"))
        if not isinstance(certification, dict):
            raise ValueError("certification artifact is not a JSON object")

        corpus_payload_sha = generation_summary.get("payload_sha256")
        store_payload_sha = (
            import_summary.get("store", {}).get("payload_sha256")
            if isinstance(import_summary.get("store"), dict)
            else None
        )
        if not corpus_payload_sha or corpus_payload_sha != store_payload_sha:
            raise RuntimeError("prepared store payload hash differs from generated corpus")

        manifest = {
            "schema": "zevryon.m5.physical-frame-run.v1",
            "workload": {
                "logical_bytes": logical_bytes,
                "records": 2,
                "giant_record_bytes": args.giant_record_bytes,
                "giant_record_index": 0,
                "checkpoint_record_index": 0,
                "checkpoint_width_px": args.width_px,
                "checkpoint_stride_kib": DEFAULT_STRIDE_KIB,
            },
            "probe": {
                "samples": args.samples,
                "warmup": args.warmup,
                "width_px": args.width_px,
                "height_px": args.height_px,
                "overscan_px": args.overscan_px,
                "max_fragments": args.max_fragments,
                "step_px": args.step_px,
            },
            "artifacts": {
                "corpus_sha256": _sha256(corpus),
                "samples_sha256": _sha256(samples) if samples.is_file() else None,
                "evidence_sha256": _sha256(evidence),
            },
            "generation": generation_summary,
            "import": import_summary,
            "verify": verify_summary,
            "arena": arena_summary,
            "checkpoint": checkpoint_summary,
            "certification": certification,
            "commands": commands,
        }
        manifest_path.write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )

        checks = certification.get("checks")
        certified_flag = bool(
            isinstance(checks, dict) and checks.get("native_frame_certified", False)
        )
        print(
            f"physical_frame_certified={str(certified_flag).lower()} "
            f"evidence={evidence} manifest={manifest_path}"
        )
        if certified.returncode != 0 or not certified_flag:
            if certified.stdout:
                print(certified.stdout.rstrip(), file=sys.stderr)
            if certified.stderr:
                print(certified.stderr.rstrip(), file=sys.stderr)
            return 2
        return 0
    except (
        OSError,
        ValueError,
        RuntimeError,
        subprocess.TimeoutExpired,
        json.JSONDecodeError,
    ) as exc:
        print(f"physical frame certification failed: {exc}", file=sys.stderr)
        return 1
    finally:
        if not args.keep_work_dir and manifest_path.exists():
            # Keep certification evidence and manifest by default; large corpus/store
            # cleanup is intentionally conservative and happens only after a manifest
            # exists so failure diagnostics are not destroyed mid-run.
            corpus.unlink(missing_ok=True)
            corpus.with_suffix(corpus.suffix + ".json").unlink(missing_ok=True)
            if store.exists():
                shutil.rmtree(store, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
