#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path
from typing import Any

from z7_html5lib_tokenizer_corpus_verify import VerificationError, verify_manifest

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MANIFEST = ROOT / "config/z7_html5lib_tokenizer_corpus.json"
DEFAULT_FIXTURE = ROOT / "tests/fixtures/html5lib-tokenizer/contentModelFlags.test"
REPORT_SCHEMA = "zevryon.z7.html5lib-tokenizer-runner.v1"

RUNNER_UPSTREAM_PATH = "tokenizer/contentModelFlags.test"
RUNNER_VENDORED_PATH = "tests/fixtures/html5lib-tokenizer/contentModelFlags.test"
RUNNER_GIT_BLOB = "9cf7c8bd9e70dfbd0037726d6a840e67d3aa5e12"
RUNNER_SIZE_BYTES = 3055
RUNNER_SHA256 = "77784a505a528950761cfb3c76617afade28b27c3be2a8c37dce3c3d8988391d"
RUNNER_TEST_COUNT = 14
RUNNER_EXECUTION_COUNT = 24

STATE_MAP = {
    "PLAINTEXT state": "PLAINTEXT",
    "RCDATA state": "RCDATA",
    "RAWTEXT state": "RAWTEXT",
}
TOKEN_MAP = {
    "Character": "C",
    "EndTag": "E",
}


class RunnerError(ValueError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RunnerError(message)


def utf8_hex(value: str) -> str:
    return value.encode("utf-8").hex()


def decode_hex(value: str, label: str) -> str:
    try:
        return bytes.fromhex(value).decode("utf-8")
    except (ValueError, UnicodeError) as exc:
        raise RunnerError(f"{label} contains invalid UTF-8 hex: {exc}") from exc


def require_canonical_authority_paths(manifest: Path, fixture: Path) -> tuple[Path, Path]:
    resolved_manifest = manifest.resolve()
    resolved_fixture = fixture.resolve()
    require(
        resolved_manifest == DEFAULT_MANIFEST.resolve(),
        "v1 runner only admits the canonical tokenizer corpus manifest",
    )
    require(
        resolved_fixture == DEFAULT_FIXTURE.resolve(),
        "v1 runner only admits the canonical pinned tokenizer fixture",
    )
    return resolved_manifest, resolved_fixture


def require_runner_fixture_provenance(manifest: Path) -> None:
    try:
        payload = json.loads(manifest.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise RunnerError(f"cannot read tokenizer provenance manifest: {exc}") from exc

    require(isinstance(payload, dict), "tokenizer provenance manifest root must be an object")
    files = payload.get("files")
    require(isinstance(files, list), "tokenizer provenance manifest files must be an array")

    matches = [
        entry
        for entry in files
        if isinstance(entry, dict)
        and entry.get("upstream_path") == RUNNER_UPSTREAM_PATH
        and entry.get("vendored_path") == RUNNER_VENDORED_PATH
    ]
    require(
        len(matches) == 1,
        "v1 runner requires exactly one pinned contentModelFlags provenance entry",
    )
    entry = matches[0]
    require(entry.get("git_blob") == RUNNER_GIT_BLOB, "runner fixture Git blob pin mismatch")
    require(entry.get("size_bytes") == RUNNER_SIZE_BYTES, "runner fixture byte-size pin mismatch")
    require(entry.get("sha256") == RUNNER_SHA256, "runner fixture SHA-256 pin mismatch")
    require(entry.get("test_count") == RUNNER_TEST_COUNT, "runner fixture test-count pin mismatch")
    require(
        entry.get("execution_count") == RUNNER_EXECUTION_COUNT,
        "runner fixture execution-count pin mismatch",
    )


def load_fixture(path: Path) -> list[dict[str, Any]]:
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise RunnerError(f"cannot read tokenizer fixture: {exc}") from exc
    require(isinstance(payload, dict), "tokenizer fixture root must be an object")
    tests = payload.get("tests")
    require(isinstance(tests, list), "tokenizer fixture tests must be an array")
    require(
        len(tests) == RUNNER_TEST_COUNT,
        f"v1 runner authority requires exactly {RUNNER_TEST_COUNT} test objects",
    )
    for index, test in enumerate(tests):
        require(isinstance(test, dict), f"test[{index}] must be an object")
    return tests


def expected_tokens(test: dict[str, Any], label: str) -> list[tuple[str, str]] | None:
    output = test.get("output")
    require(isinstance(output, list), f"{label} output must be an array")
    result: list[tuple[str, str]] = []
    for index, raw in enumerate(output):
        require(
            isinstance(raw, list) and len(raw) == 2,
            f"{label} output[{index}] must be a two-item token array",
        )
        kind, value = raw
        require(
            isinstance(kind, str) and isinstance(value, str),
            f"{label} output[{index}] token fields must be strings",
        )
        mapped = TOKEN_MAP.get(kind)
        if mapped is None:
            return None
        result.append((mapped, utf8_hex(value)))
    return result


def expected_errors(test: dict[str, Any], label: str) -> list[tuple[str, int, int]]:
    raw_errors = test.get("errors", [])
    require(isinstance(raw_errors, list), f"{label} errors must be an array")
    result: list[tuple[str, int, int]] = []
    for index, raw in enumerate(raw_errors):
        require(isinstance(raw, dict), f"{label} errors[{index}] must be an object")
        code = raw.get("code")
        line = raw.get("line")
        column = raw.get("col")
        require(isinstance(code, str) and code, f"{label} errors[{index}] code invalid")
        require(
            isinstance(line, int) and not isinstance(line, bool) and line >= 1,
            f"{label} errors[{index}] line invalid",
        )
        require(
            isinstance(column, int) and not isinstance(column, bool) and column >= 1,
            f"{label} errors[{index}] col invalid",
        )
        result.append((utf8_hex(code), line, column))
    return result


def parse_probe_output(stdout: str) -> tuple[
    list[tuple[str, str]],
    list[tuple[str, int, int]],
    tuple[int, int, int, int, int, int],
]:
    tokens: list[tuple[str, str]] = []
    errors: list[tuple[str, int, int]] = []
    stats: tuple[int, int, int, int, int, int] | None = None
    for line_number, line in enumerate(stdout.splitlines(), start=1):
        fields = line.split("\t")
        require(fields and fields[0], f"probe output line {line_number} is empty")
        if fields[0] == "TOKEN":
            require(len(fields) == 3, f"probe TOKEN line {line_number} malformed")
            require(fields[1] in {"C", "E"}, f"probe token kind line {line_number} invalid")
            require(len(fields[2]) % 2 == 0, f"probe token hex line {line_number} invalid")
            try:
                bytes.fromhex(fields[2])
            except ValueError as exc:
                raise RunnerError(f"probe token hex line {line_number} invalid") from exc
            tokens.append((fields[1], fields[2].lower()))
        elif fields[0] == "ERROR":
            require(len(fields) == 4, f"probe ERROR line {line_number} malformed")
            try:
                bytes.fromhex(fields[1])
                line_value = int(fields[2], 10)
                column_value = int(fields[3], 10)
            except ValueError as exc:
                raise RunnerError(f"probe ERROR line {line_number} invalid") from exc
            require(
                line_value >= 1 and column_value >= 1,
                f"probe ERROR line {line_number} position invalid",
            )
            errors.append((fields[1].lower(), line_value, column_value))
        elif fields[0] == "STATS":
            require(len(fields) == 7, f"probe STATS line {line_number} malformed")
            require(stats is None, "probe emitted duplicate STATS line")
            try:
                values = tuple(int(value, 10) for value in fields[1:])
            except ValueError as exc:
                raise RunnerError(f"probe STATS line {line_number} invalid") from exc
            require(
                len(values) == 6 and all(value >= 0 for value in values),
                f"probe STATS line {line_number} contains invalid counters",
            )
            stats = values  # type: ignore[assignment]
        elif fields[0] == "FAIL":
            require(len(fields) == 2, f"probe FAIL line {line_number} malformed")
            raise RunnerError(f"probe tokenizer failure: {decode_hex(fields[1], 'probe FAIL')}")
        else:
            raise RunnerError(f"unknown probe output record on line {line_number}")
    require(stats is not None, "probe output is missing STATS line")
    return tokens, errors, stats


def run_execution(
    probe: Path,
    state: str,
    last_start_tag: str,
    input_text: str,
    expected_token_stream: list[tuple[str, str]],
    expected_error_stream: list[tuple[str, int, int]],
    label: str,
) -> None:
    input_bytes = input_text.encode("utf-8")
    command = [str(probe), state, utf8_hex(last_start_tag), input_bytes.hex()]
    try:
        completed = subprocess.run(
            command,
            check=False,
            capture_output=True,
            text=True,
            timeout=10,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise RunnerError(f"{label} probe execution failed: {exc}") from exc

    if completed.returncode != 0:
        detail = completed.stdout.strip() or completed.stderr.strip() or "no probe detail"
        if detail.startswith("FAIL\t"):
            fields = detail.split("\t", 1)
            if len(fields) == 2:
                detail = decode_hex(fields[1], f"{label} FAIL")
        raise RunnerError(f"{label} probe returned {completed.returncode}: {detail}")

    require(not completed.stderr.strip(), f"{label} probe emitted unexpected stderr")
    actual_tokens, actual_errors, stats = parse_probe_output(completed.stdout)
    require(actual_tokens == expected_token_stream, f"{label} token stream mismatch")
    require(actual_errors == expected_error_stream, f"{label} parse-error stream mismatch")

    character_tokens = sum(1 for kind, _ in expected_token_stream if kind == "C")
    character_bytes = sum(
        len(bytes.fromhex(value))
        for kind, value in expected_token_stream
        if kind == "C"
    )
    end_tags = sum(1 for kind, _ in expected_token_stream if kind == "E")
    expected_stats = (
        len(input_bytes),
        len(expected_token_stream),
        character_tokens,
        character_bytes,
        end_tags,
        len(expected_error_stream),
    )
    require(stats == expected_stats, f"{label} tokenizer stats mismatch")


def execute_fixture(probe: Path, fixture: Path) -> dict[str, Any]:
    require(probe.is_file(), f"tokenizer probe is missing: {probe}")
    tests = load_fixture(fixture)

    passed = 0
    failed = 0
    unsupported = 0
    failures: list[str] = []
    execution_index = 0

    for test_index, test in enumerate(tests):
        description = test.get("description")
        require(
            isinstance(description, str) and description,
            f"test[{test_index}] description invalid",
        )
        label = f"test[{test_index}] {description}"
        states = test.get("initialStates")
        require(isinstance(states, list) and states, f"{label} initialStates invalid")
        last_start_tag = test.get("lastStartTag", "")
        input_text = test.get("input")
        require(isinstance(last_start_tag, str), f"{label} lastStartTag invalid")
        require(isinstance(input_text, str), f"{label} input invalid")

        token_stream = expected_tokens(test, label)
        error_stream = expected_errors(test, label)
        double_escaped = test.get("doubleEscaped", False)
        require(isinstance(double_escaped, bool), f"{label} doubleEscaped invalid")

        for raw_state in states:
            execution_index += 1
            require(isinstance(raw_state, str), f"{label} initial state must be string")
            execution_label = f"{label} execution[{execution_index}] {raw_state}"
            state = STATE_MAP.get(raw_state)
            if state is None or token_stream is None or double_escaped:
                unsupported += 1
                failures.append(f"UNSUPPORTED: {execution_label}")
                continue
            if not input_text.isascii() or not last_start_tag.isascii():
                unsupported += 1
                failures.append(
                    f"UNSUPPORTED: {execution_label} requires non-ASCII input authority"
                )
                continue
            try:
                run_execution(
                    probe,
                    state,
                    last_start_tag,
                    input_text,
                    token_stream,
                    error_stream,
                    execution_label,
                )
            except RunnerError as exc:
                failed += 1
                failures.append(f"FAIL: {exc}")
            else:
                passed += 1

    require(
        execution_index == RUNNER_EXECUTION_COUNT,
        f"v1 runner authority requires exactly {RUNNER_EXECUTION_COUNT} executions",
    )
    report = {
        "schema": REPORT_SCHEMA,
        "fixture": DEFAULT_FIXTURE.relative_to(ROOT).as_posix(),
        "fixture_git_blob": RUNNER_GIT_BLOB,
        "tests": len(tests),
        "executions": execution_index,
        "passed": passed,
        "failed": failed,
        "unsupported": unsupported,
        "fixture_pass_claim": failed == 0 and unsupported == 0,
        "html_tokenizer_conformance_claim": False,
        "z7_status_change": False,
    }
    if failures:
        for failure in failures:
            print(failure, file=sys.stderr)
    return report


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", required=True, type=Path)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--fixture", type=Path, default=DEFAULT_FIXTURE)
    args = parser.parse_args()

    try:
        manifest, fixture = require_canonical_authority_paths(args.manifest, args.fixture)
        provenance = verify_manifest(manifest, ROOT)
        require(
            provenance.get("provenance_gate_passed") is True,
            "aggregate tokenizer provenance gate did not pass",
        )
        require(
            provenance.get("conformance_claim") is False,
            "provenance verifier unexpectedly claims tokenizer conformance",
        )
        require_runner_fixture_provenance(manifest)
        report = execute_fixture(args.probe.resolve(), fixture)
    except (RunnerError, VerificationError) as exc:
        print(f"Z7 html5lib tokenizer runner failed: {exc}", file=sys.stderr)
        return 1

    print(json.dumps(report, sort_keys=True))
    if report["failed"] != 0 or report["unsupported"] != 0:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
