#!/usr/bin/env python3
from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROBE = (ROOT / "cmake/z2r3d_unicode_probe.cmake").as_posix()
PREREQUISITE = "2a717a6a48ffbbd8c0b3d25f35c4320af41fdfed"


def run(*args: str) -> None:
    subprocess.run(args, cwd=ROOT, check=True)


def configure(build: str, *, shadow: bool, strict: bool, hooks: bool) -> None:
    values = {
        "ZEVRYON_ENABLE_RUST_CORE": False,
        "ZEVRYON_RUST_UNICODE_SHADOW": shadow,
        "ZEVRYON_RUST_UNICODE_SHADOW_STRICT": strict,
        "ZEVRYON_RUST_UNICODE_SHADOW_TEST_HOOKS": hooks,
        "ZEVRYON_RUST_LEDGER_SHADOW": False,
        "ZEVRYON_RUST_LEDGER_AUTHORITATIVE": False,
        "ZEVRYON_ENABLE_FONTCONFIG_DISCOVERY": False,
        "ZEVRYON_ENABLE_DIRECTWRITE_DISCOVERY": False,
    }
    args = [
        "cmake", "-S", ".", "-B", build,
        "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_TESTING=OFF",
        f"-DCMAKE_PROJECT_INCLUDE={PROBE}",
    ]
    args.extend(f"-D{k}={'ON' if v else 'OFF'}" for k, v in values.items())
    run(*args)


def build(build_dir: str) -> Path:
    run(
        "cmake", "--build", build_dir, "--config", "Release", "--parallel", "2",
        "--target", "zevryon-z2r3du-unicode-workload",
    )
    name = "zevryon-z2r3du-unicode-workload.exe" if os.name == "nt" else "zevryon-z2r3du-unicode-workload"
    prefix = Path(build_dir) / ("Release" if os.name == "nt" else "")
    return (ROOT / prefix / name).resolve()


def platform(args: argparse.Namespace) -> None:
    baseline_dir = "build-z2r3du-baseline"
    shadow_dir = "build-z2r3du-shadow"
    fault_dir = "build-z2r3du-fault"
    configure(baseline_dir, shadow=False, strict=False, hooks=False)
    baseline = build(baseline_dir)
    if (ROOT / baseline_dir / "rust-target").exists():
        raise RuntimeError("Rust target exists in rollback build")
    configure(shadow_dir, shadow=True, strict=True, hooks=False)
    shadow = build(shadow_dir)
    fault: Path | None = None
    if args.platform == "linux":
        configure(fault_dir, shadow=True, strict=False, hooks=True)
        fault = build(fault_dir)

    evidence = ROOT / "evidence/z2r3du"
    evidence.mkdir(parents=True, exist_ok=True)
    paired = evidence / f"{args.platform}-paired.json"
    command = [
        sys.executable, "scripts/z2r3d_run_unicode_workloads.py",
        "--baseline-binary", str(baseline), "--shadow-binary", str(shadow),
        "--platform", args.platform, "--logical-bytes", "16777216",
        "--rounds", "2", "--samples", "3", "--output", str(paired),
    ]
    if fault is not None:
        command.extend(["--fault-binary", str(fault)])
    run(*command)
    run(
        sys.executable, "scripts/z2r3d_unicode_certification.py",
        "--report", str(paired), "--commit-sha", args.sha,
        "--compiler", args.compiler, "--build-type", "Release",
        "--max-p50-ratio", "2.00", "--max-p95-ratio", "2.25",
        "--max-p99-ratio", "2.50", "--max-maximum-ratio", "3.00",
        "--max-memory-ratio", "1.50", "--max-memory-delta-bytes", "16777216",
        "--output", str(evidence / f"{args.platform}-certification.json"),
    )


def finalize(args: argparse.Namespace) -> None:
    base = ROOT / "evidence/z2r3du/downloaded"
    out = ROOT / "evidence/z2r3du/z2r3du-promotion-readiness.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    run(
        sys.executable, "scripts/z2r3d_finalize_unicode_promotion.py",
        "--linux", str(base / "linux-certification.json"),
        "--windows", str(base / "windows-certification.json"),
        "--macos", str(base / "macos-certification.json"),
        "--commit-sha", args.sha, "--prerequisite-head", PREREQUISITE,
        "--output", str(out),
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("platform")
    p.add_argument("--platform", choices=("linux", "windows", "macos"), required=True)
    p.add_argument("--compiler", required=True)
    p.add_argument("--sha", required=True)
    p.set_defaults(func=platform)
    f = sub.add_parser("finalize")
    f.add_argument("--sha", required=True)
    f.set_defaults(func=finalize)
    args = parser.parse_args()
    args.func(args)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
