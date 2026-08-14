#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import platform as host_platform
import shutil
import subprocess
import sys
import time
from pathlib import Path
from typing import Sequence

ROOT = Path(__file__).resolve().parents[1]
SUPPORTED_HOSTS = {
    "linux": "Linux",
    "windows": "Windows",
}
REGRESSION_TESTS = (
    "unicode-stream-tests",
    "grapheme-segmenter-tests",
    "script-run-tests",
    "bidi-explicit-tests",
)
AUTHORITY_TESTS = (
    "unicode-authority-positive",
    "unicode-authority-fault-output",
    "unicode-authority-fault-error",
    "unicode-authority-fault-state",
    "unicode-authority-fault-reset",
)


def executable(name: str) -> str:
    resolved = shutil.which(name)
    if resolved is None:
        raise RuntimeError(f"required executable not found: {name}")
    return resolved


def exact_head(expected: str) -> str:
    actual = subprocess.check_output(
        ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True
    ).strip()
    if actual != expected:
        raise RuntimeError(f"exact-head mismatch: expected {expected}, got {actual}")
    return actual


def require_clean_checkout() -> None:
    tracked = subprocess.run(
        ["git", "status", "--porcelain", "--untracked-files=no"],
        cwd=ROOT,
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    if tracked.strip():
        raise RuntimeError("tracked checkout is not clean")


def require_supported_host(declared_platform: str) -> None:
    expected_host = SUPPORTED_HOSTS[declared_platform]
    actual_host = host_platform.system()
    if actual_host != expected_host:
        raise RuntimeError(
            "platform mismatch: "
            f"declared {declared_platform}, expected host {expected_host}, got {actual_host}"
        )


def run(command: Sequence[str], *, log: Path) -> None:
    printable = subprocess.list2cmdline(list(command))
    print(f"\n=== {printable} ===", flush=True)
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open("w", encoding="utf-8", newline="\n") as handle:
        handle.write(f"$ {printable}\n")
        process = subprocess.Popen(
            list(command),
            cwd=ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
        assert process.stdout is not None
        for line in process.stdout:
            print(line, end="")
            handle.write(line)
        code = process.wait()
    if code != 0:
        raise subprocess.CalledProcessError(code, list(command))


def remove_build(path: Path) -> None:
    if path.exists():
        shutil.rmtree(path)


def cmake_values(
    *, authoritative: bool, strict: bool, hooks: bool
) -> dict[str, bool]:
    return {
        "BUILD_TESTING": True,
        "ZEVRYON_ENABLE_RUST_CORE": False,
        "ZEVRYON_RUST_UNICODE_AUTHORITATIVE": authoritative,
        "ZEVRYON_RUST_UNICODE_SHADOW": False,
        "ZEVRYON_RUST_UNICODE_SHADOW_STRICT": strict,
        "ZEVRYON_RUST_UNICODE_SHADOW_TEST_HOOKS": False,
        "ZEVRYON_RUST_UNICODE_AUTHORITY_TEST_HOOKS": hooks,
        "ZEVRYON_RUST_LEDGER_SHADOW": False,
        "ZEVRYON_RUST_LEDGER_AUTHORITATIVE": False,
        "ZEVRYON_RUST_MASSIVEDOC_CODEC_SHADOW": False,
        "ZEVRYON_RUST_MASSIVEDOC_CODEC_AUTHORITATIVE": False,
        "ZEVRYON_ENABLE_FONTCONFIG_DISCOVERY": False,
        "ZEVRYON_ENABLE_DIRECTWRITE_DISCOVERY": False,
    }


def configure(
    build: Path,
    *,
    authoritative: bool,
    strict: bool,
    hooks: bool,
    log: Path,
) -> None:
    values = cmake_values(
        authoritative=authoritative,
        strict=strict,
        hooks=hooks,
    )
    command = [
        executable("cmake"),
        "-S",
        ".",
        "-B",
        str(build),
        "-DCMAKE_BUILD_TYPE=Release",
    ]
    command.extend(
        f"-D{name}={'ON' if enabled else 'OFF'}"
        for name, enabled in values.items()
    )
    run(command, log=log)


def build_targets(build: Path, targets: Sequence[str], *, log: Path) -> None:
    run(
        [
            executable("cmake"),
            "--build",
            str(build),
            "--config",
            "Release",
            "--parallel",
            "2",
            "--target",
            *targets,
        ],
        log=log,
    )


def ctest(build: Path, tests: Sequence[str], *, log: Path) -> None:
    expression = "^(" + "|".join(tests) + ")$"
    run(
        [
            executable("ctest"),
            "--test-dir",
            str(build),
            "-C",
            "Release",
            "--output-on-failure",
            "-R",
            expression,
        ],
        log=log,
    )


def validate_rust(log_dir: Path) -> None:
    cargo = executable("cargo")
    manifest = str(ROOT / "rust" / "Cargo.toml")
    run(
        [cargo, "fmt", "--manifest-path", manifest, "--all", "--", "--check"],
        log=log_dir / "rust-fmt.log",
    )
    run(
        [cargo, "test", "--manifest-path", manifest, "--workspace", "--locked"],
        log=log_dir / "rust-test.log",
    )
    run(
        [
            cargo,
            "clippy",
            "--manifest-path",
            manifest,
            "--workspace",
            "--all-targets",
            "--locked",
            "--",
            "-D",
            "warnings",
        ],
        log=log_dir / "rust-clippy.log",
    )


def validate_default_cpp(log_dir: Path) -> None:
    build = ROOT / "build-z2r3eu-default"
    remove_build(build)
    configure(
        build,
        authoritative=False,
        strict=False,
        hooks=False,
        log=log_dir / "default-configure.log",
    )
    if (build / "rust-target").exists():
        raise RuntimeError("rollback build created rust-target")
    build_targets(
        build,
        tuple(f"zevryon-{name}" for name in REGRESSION_TESTS),
        log=log_dir / "default-build.log",
    )
    ctest(build, REGRESSION_TESTS, log=log_dir / "default-ctest.log")
    if (build / "rust-target").exists():
        raise RuntimeError("rollback build used Cargo")


def validate_strict_authority(log_dir: Path) -> None:
    build = ROOT / "build-z2r3eu-authority"
    remove_build(build)
    configure(
        build,
        authoritative=True,
        strict=True,
        hooks=False,
        log=log_dir / "authority-configure.log",
    )
    targets = ("zevryon-unicode-authority-tests",) + tuple(
        f"zevryon-{name}" for name in REGRESSION_TESTS
    )
    build_targets(build, targets, log=log_dir / "authority-build.log")
    ctest(
        build,
        ("unicode-authority-positive",) + REGRESSION_TESTS,
        log=log_dir / "authority-ctest.log",
    )


def validate_diagnostic_faults(log_dir: Path) -> None:
    build = ROOT / "build-z2r3eu-diagnostic"
    remove_build(build)
    configure(
        build,
        authoritative=True,
        strict=False,
        hooks=True,
        log=log_dir / "diagnostic-configure.log",
    )
    build_targets(
        build,
        ("zevryon-unicode-authority-tests",),
        log=log_dir / "diagnostic-build.log",
    )
    ctest(build, AUTHORITY_TESTS, log=log_dir / "diagnostic-ctest.log")


def tool_version(command: Sequence[str]) -> str:
    return subprocess.check_output(
        list(command), cwd=ROOT, text=True, stderr=subprocess.STDOUT
    ).splitlines()[0]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--sha", required=True)
    parser.add_argument(
        "--platform",
        choices=tuple(SUPPORTED_HOSTS),
        required=True,
    )
    parser.add_argument("--compiler", required=True)
    parser.add_argument(
        "--output",
        type=Path,
        default=None,
    )
    args = parser.parse_args()

    started = time.time()
    summary_path = args.output or (
        ROOT
        / "evidence"
        / "z2r3eu"
        / f"{args.platform}-authority-validation.json"
    )
    log_dir = summary_path.parent / f"{args.platform}-logs"
    summary_path.parent.mkdir(parents=True, exist_ok=True)

    summary: dict[str, object] = {
        "schema": "zevryon.z2r3eu.authority-validation.v1",
        "commit_sha": args.sha,
        "platform": args.platform,
        "compiler": args.compiler,
        "host": host_platform.platform(),
        "python": sys.version.split()[0],
        "success": False,
        "rollback_cargo_free": False,
        "rust_workspace_passed": False,
        "strict_authority_passed": False,
        "diagnostic_faults_passed": False,
    }

    try:
        executable("git")
        executable("cmake")
        executable("ctest")
        exact_head(args.sha)
        require_clean_checkout()
        require_supported_host(args.platform)
        summary["cmake"] = tool_version(["cmake", "--version"])
        summary["cargo"] = tool_version(["cargo", "--version"])
        summary["rustc"] = tool_version(["rustc", "--version"])

        validate_rust(log_dir)
        summary["rust_workspace_passed"] = True

        validate_default_cpp(log_dir)
        summary["rollback_cargo_free"] = True

        validate_strict_authority(log_dir)
        summary["strict_authority_passed"] = True

        validate_diagnostic_faults(log_dir)
        summary["diagnostic_faults_passed"] = True
        summary["success"] = True
    except BaseException as error:
        summary["error_type"] = type(error).__name__
        summary["error"] = str(error)
        raise
    finally:
        summary["duration_seconds"] = round(time.time() - started, 3)
        summary_path.write_text(
            json.dumps(summary, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        print(f"validation summary: {summary_path}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
