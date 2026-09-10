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
DEFAULT_FIXTURE = ROOT / "tests/fixtures/html5lib-tokenizer/test1.test"
REPORT_SCHEMA = "zevryon.z7.html5lib-tokenizer-test1-admitted-runner.v1"

RUNNER_UPSTREAM_PATH = "tokenizer/test1.test"
RUNNER_VENDORED_PATH = "tests/fixtures/html5lib-tokenizer/test1.test"
RUNNER_GIT_BLOB = "5323fbbeae2c6116aab14a716c8df1174e7870fb"
RUNNER_SIZE_BYTES = 10006
RUNNER_SHA256 = "524fcfa4d561a14f0c4e72e0573549abe6341fd4dfb8e16bc2dcf59a608a7219"
RUNNER_TEST_COUNT = 69
RUNNER_EXECUTION_COUNT = 69
ADMITTED_EXECUTION_COUNT = 48
UNSUPPORTED_EXECUTION_COUNT = 21

STATE_MAP = {
    "Data state": "DATA",
    "Script data state": "SCRIPT_DATA",
}

ADMITTED_DESCRIPTIONS = frozenset(
    {
        # DOCTYPE / bogus-comment declaration surface.
        "Correct Doctype lowercase",
        "Correct Doctype uppercase",
        "Correct Doctype mixed case",
        "Correct Doctype case with EOF",
        "Truncated doctype start",
        "Doctype in error",
        # Data tag / attribute surface.
        "Single Start Tag",
        "Empty end tag",
        "Empty start tag",
        "Start Tag w/attribute",
        "Start Tag w/attribute no quotes",
        "Start/End Tag",
        "Two unclosed start tags",
        "End Tag w/attribute",
        "Multiple atts",
        "Multiple atts no space",
        "Repeated attr",
        "Open angled bracket in unquoted attribute value state",
        # Comment state-family surface.
        "Simple comment",
        "Comment, Central dash no space",
        "Comment, two central dashes",
        "Comment, central less-than bang",
        "Unfinished comment",
        "Unfinished comment after start of nested comment",
        "Start of a comment",
        "Short comment",
        "Short comment two",
        "Short comment three",
        "< in comment",
        "<< in comment",
        "<! in comment",
        "<!- in comment",
        "Nested comment",
        "Nested comment with extra <",
        # Script-data state-family surface.
        "< in script data",
        "<! in script data",
        "<!- in script data",
        "Escaped script data",
        "< in script HTML comment",
        "</ in script HTML comment",
        "Start tag in script HTML comment",
        "End tag in script HTML comment",
        "- in script HTML comment double escaped",
        "-- in script HTML comment double escaped",
        "--- in script HTML comment double escaped",
        "- spaced in script HTML comment double escaped",
        "-- spaced in script HTML comment double escaped",
        # Token-stream-only observation. The tokenizer intentionally does not
        # fake tree-builder feedback after emitting the plaintext start tag.
        "plaintext element",
    }
)


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
        "test1 admitted runner only accepts the canonical tokenizer manifest",
    )
    require(
        resolved_fixture == DEFAULT_FIXTURE.resolve(),
        "test1 admitted runner only accepts the canonical pinned test1 fixture",
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
    require(len(matches) == 1, "runner requires exactly one pinned test1 provenance entry")
    entry = matches[0]
    require(entry.get("git_blob") == RUNNER_GIT_BLOB, "test1 Git blob pin mismatch")
    require(entry.get("size_bytes") == RUNNER_SIZE_BYTES, "test1 byte-size pin mismatch")
    require(entry.get("sha256") == RUNNER_SHA256, "test1 SHA-256 pin mismatch")
    require(entry.get("test_count") == RUNNER_TEST_COUNT, "test1 test-count pin mismatch")
    require(
        entry.get("execution_count") == RUNNER_EXECUTION_COUNT,
        "test1 execution-count pin mismatch",
    )


def load_fixture(path: Path) -> list[dict[str, Any]]:
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise RunnerError(f"cannot read tokenizer fixture: {exc}") from exc
    require(isinstance(payload, dict), "test1 fixture root must be an object")
    tests = payload.get("tests")
    require(isinstance(tests, list), "test1 fixture tests must be an array")
    require(len(tests) == RUNNER_TEST_COUNT, "test1 fixture must contain exactly 69 tests")
    for index, test in enumerate(tests):
        require(isinstance(test, dict), f"test[{index}] must be an object")
    return tests


def fixture_partition(tests: list[dict[str, Any]]) -> tuple[list[str], list[str]]:
    descriptions: list[str] = []
    for index, test in enumerate(tests):
        description = test.get("description")
        require(
            isinstance(description, str) and description,
            f"test[{index}] description must be a non-empty string",
        )
        descriptions.append(description)
    require(len(descriptions) == len(set(descriptions)), "test1 descriptions must be unique")
    admitted = [description for description in descriptions if description in ADMITTED_DESCRIPTIONS]
    unsupported = [description for description in descriptions if description not in ADMITTED_DESCRIPTIONS]
    require(
        set(admitted) == ADMITTED_DESCRIPTIONS,
        "test1 admitted description allowlist drifted from the pinned fixture",
    )
    require(
        len(admitted) == ADMITTED_EXECUTION_COUNT,
        f"test1 admitted denominator must remain exactly {ADMITTED_EXECUTION_COUNT}",
    )
    require(
        len(unsupported) == UNSUPPORTED_EXECUTION_COUNT,
        f"test1 unsupported denominator must remain exactly {UNSUPPORTED_EXECUTION_COUNT}",
    )
    return admitted, unsupported


def expected_token(raw: Any, label: str) -> tuple[Any, ...]:
    require(isinstance(raw, list) and raw, f"{label} must be a non-empty token array")
    kind = raw[0]
    require(isinstance(kind, str), f"{label} token kind must be a string")

    if kind == "Character":
        require(len(raw) == 2 and isinstance(raw[1], str), f"{label} Character shape invalid")
        return ("C", raw[1])
    if kind == "EndTag":
        require(len(raw) == 2 and isinstance(raw[1], str), f"{label} EndTag shape invalid")
        return ("E", raw[1])
    if kind == "Comment":
        require(len(raw) == 2 and isinstance(raw[1], str), f"{label} Comment shape invalid")
        return ("M", raw[1])
    if kind == "StartTag":
        require(len(raw) in {3, 4}, f"{label} StartTag length invalid")
        require(isinstance(raw[1], str) and isinstance(raw[2], dict), f"{label} StartTag invalid")
        attrs: list[tuple[str, str]] = []
        for name, value in raw[2].items():
            require(
                isinstance(name, str) and isinstance(value, str),
                f"{label} StartTag attributes must be string-to-string",
            )
            attrs.append((name, value))
        self_closing = False
        if len(raw) == 4:
            require(raw[3] is True, f"{label} self-closing flag must be true when present")
            self_closing = True
        return ("S", raw[1], self_closing, tuple(sorted(attrs)))
    if kind == "DOCTYPE":
        require(len(raw) == 5, f"{label} DOCTYPE length invalid")
        name, public_id, system_id, correct = raw[1:]
        require(isinstance(name, str), f"{label} DOCTYPE name invalid")
        require(public_id is None or isinstance(public_id, str), f"{label} public id invalid")
        require(system_id is None or isinstance(system_id, str), f"{label} system id invalid")
        require(isinstance(correct, bool), f"{label} correctness flag invalid")
        # html5lib serializes the token's correctness bit; the production probe
        # serializes force_quirks. They are logical inverses.
        return ("D", name, public_id, system_id, not correct)
    raise RunnerError(f"{label} token kind is unsupported by the admitted runner: {kind!r}")


def expected_tokens(test: dict[str, Any], label: str) -> list[tuple[Any, ...]]:
    output = test.get("output")
    require(isinstance(output, list), f"{label} output must be an array")
    return [expected_token(raw, f"{label} output[{index}]") for index, raw in enumerate(output)]


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
        result.append((code, line, column))
    return result


def parse_bool_field(value: str, label: str) -> bool:
    require(value in {"0", "1"}, f"{label} must be 0 or 1")
    return value == "1"


def parse_probe_output(stdout: str) -> tuple[
    list[tuple[Any, ...]],
    list[tuple[str, int, int]],
    tuple[int, int, int, int, int, int],
]:
    tokens: list[tuple[Any, ...]] = []
    errors: list[tuple[str, int, int]] = []
    stats: tuple[int, int, int, int, int, int] | None = None

    for line_number, line in enumerate(stdout.splitlines(), start=1):
        fields = line.split("\t")
        require(fields and fields[0], f"probe output line {line_number} is empty")
        if fields[0] == "TOKEN":
            require(len(fields) >= 3, f"probe TOKEN line {line_number} malformed")
            kind = fields[1]
            if kind in {"C", "E", "M"}:
                require(len(fields) == 3, f"probe {kind} line {line_number} malformed")
                tokens.append((kind, decode_hex(fields[2], f"probe token line {line_number}")))
                continue
            if kind == "S":
                require(len(fields) >= 5, f"probe StartTag line {line_number} malformed")
                name = decode_hex(fields[2], f"probe StartTag name line {line_number}")
                self_closing = parse_bool_field(fields[3], f"probe StartTag self-closing line {line_number}")
                try:
                    attribute_count = int(fields[4], 10)
                except ValueError as exc:
                    raise RunnerError(f"probe StartTag attribute count line {line_number} invalid") from exc
                require(attribute_count >= 0, f"probe StartTag attribute count line {line_number} invalid")
                require(
                    len(fields) == 5 + attribute_count * 2,
                    f"probe StartTag attribute payload line {line_number} malformed",
                )
                attrs: list[tuple[str, str]] = []
                for attribute_index in range(attribute_count):
                    base = 5 + attribute_index * 2
                    attrs.append(
                        (
                            decode_hex(fields[base], f"probe attribute name line {line_number}"),
                            decode_hex(fields[base + 1], f"probe attribute value line {line_number}"),
                        )
                    )
                require(
                    len(attrs) == len(set(name for name, _ in attrs)),
                    f"probe StartTag line {line_number} contains duplicate published attributes",
                )
                tokens.append(("S", name, self_closing, tuple(sorted(attrs))))
                continue
            if kind == "D":
                require(len(fields) == 8, f"probe DOCTYPE line {line_number} malformed")
                name = decode_hex(fields[2], f"probe DOCTYPE name line {line_number}")
                has_public = parse_bool_field(fields[3], f"probe DOCTYPE public flag line {line_number}")
                public_payload = decode_hex(fields[4], f"probe DOCTYPE public id line {line_number}")
                has_system = parse_bool_field(fields[5], f"probe DOCTYPE system flag line {line_number}")
                system_payload = decode_hex(fields[6], f"probe DOCTYPE system id line {line_number}")
                force_quirks = parse_bool_field(fields[7], f"probe DOCTYPE force-quirks line {line_number}")
                require(has_public or public_payload == "", f"probe DOCTYPE absent public id has payload")
                require(has_system or system_payload == "", f"probe DOCTYPE absent system id has payload")
                tokens.append(
                    (
                        "D",
                        name,
                        public_payload if has_public else None,
                        system_payload if has_system else None,
                        force_quirks,
                    )
                )
                continue
            raise RunnerError(f"probe token kind line {line_number} invalid: {kind!r}")

        if fields[0] == "ERROR":
            require(len(fields) == 4, f"probe ERROR line {line_number} malformed")
            code = decode_hex(fields[1], f"probe ERROR code line {line_number}")
            try:
                line_value = int(fields[2], 10)
                column_value = int(fields[3], 10)
            except ValueError as exc:
                raise RunnerError(f"probe ERROR coordinates line {line_number} invalid") from exc
            require(line_value >= 1 and column_value >= 1, f"probe ERROR position line {line_number} invalid")
            errors.append((code, line_value, column_value))
            continue

        if fields[0] == "STATS":
            require(len(fields) == 7, f"probe STATS line {line_number} malformed")
            require(stats is None, "probe emitted duplicate STATS line")
            try:
                values = tuple(int(value, 10) for value in fields[1:])
            except ValueError as exc:
                raise RunnerError(f"probe STATS line {line_number} invalid") from exc
            require(len(values) == 6 and all(value >= 0 for value in values), "probe STATS contains invalid counters")
            stats = values  # type: ignore[assignment]
            continue

        if fields[0] == "FAIL":
            require(len(fields) == 2, f"probe FAIL line {line_number} malformed")
            raise RunnerError(f"probe tokenizer failure: {decode_hex(fields[1], 'probe FAIL')}")

        raise RunnerError(f"unknown probe output record on line {line_number}")

    require(stats is not None, "probe output is missing STATS line")
    return tokens, errors, stats


def expected_common_stats(
    input_text: str,
    tokens: list[tuple[Any, ...]],
    errors: list[tuple[str, int, int]],
) -> tuple[int, int, int, int, int, int]:
    character_tokens = [token for token in tokens if token[0] == "C"]
    character_bytes = sum(len(str(token[1]).encode("utf-8")) for token in character_tokens)
    end_tags = sum(1 for token in tokens if token[0] == "E")
    return (
        len(input_text.encode("utf-8")),
        len(tokens),
        len(character_tokens),
        character_bytes,
        end_tags,
        len(errors),
    )


def run_execution(
    probe: Path,
    state: str,
    last_start_tag: str,
    input_text: str,
    token_stream: list[tuple[Any, ...]],
    error_stream: list[tuple[str, int, int]],
    label: str,
) -> None:
    command = [str(probe), state, utf8_hex(last_start_tag), utf8_hex(input_text)]
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

    actual_tokens, actual_errors, actual_stats = parse_probe_output(completed.stdout)
    require(actual_tokens == token_stream, f"{label} token stream mismatch")
    require(actual_errors == error_stream, f"{label} parse-error stream mismatch")
    require(
        actual_stats == expected_common_stats(input_text, token_stream, error_stream),
        f"{label} common tokenizer stats mismatch",
    )


def execute_fixture(probe: Path, fixture: Path) -> dict[str, Any]:
    require(probe.is_file(), f"tokenizer probe is missing: {probe}")
    tests = load_fixture(fixture)
    admitted_descriptions, unsupported_descriptions = fixture_partition(tests)
    admitted_set = set(admitted_descriptions)

    passed = 0
    failed = 0
    failures: list[str] = []
    execution_count = 0

    for test_index, test in enumerate(tests):
        description = test["description"]
        states = test.get("initialStates", ["Data state"])
        require(isinstance(states, list) and states, f"test[{test_index}] initialStates invalid")
        require(len(states) == 1, f"test[{test_index}] must have exactly one execution in pinned test1")
        execution_count += 1

        if description not in admitted_set:
            continue

        raw_state = states[0]
        require(isinstance(raw_state, str), f"test[{test_index}] initial state invalid")
        state = STATE_MAP.get(raw_state)
        require(state is not None, f"admitted test {description!r} uses an unadmitted initial state")
        last_start_tag = test.get("lastStartTag", "")
        input_text = test.get("input")
        require(isinstance(last_start_tag, str), f"test[{test_index}] lastStartTag invalid")
        require(isinstance(input_text, str), f"test[{test_index}] input invalid")
        require(test.get("doubleEscaped", False) is False, f"admitted test {description!r} is doubleEscaped")
        require(input_text.isascii() and last_start_tag.isascii(), f"admitted test {description!r} is non-ASCII")

        label = f"test[{test_index}] {description} [{raw_state}]"
        token_stream = expected_tokens(test, label)
        error_stream = expected_errors(test, label)
        try:
            run_execution(
                probe,
                state,
                last_start_tag,
                input_text,
                token_stream,
                error_stream,
                label,
            )
        except RunnerError as exc:
            failed += 1
            failures.append(str(exc))
        else:
            passed += 1

    require(execution_count == RUNNER_EXECUTION_COUNT, "test1 execution denominator drifted")
    require(
        passed + failed == ADMITTED_EXECUTION_COUNT,
        "test1 admitted execution accounting drifted",
    )
    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)

    admitted_surface_pass = passed == ADMITTED_EXECUTION_COUNT and failed == 0
    return {
        "schema": REPORT_SCHEMA,
        "fixture": DEFAULT_FIXTURE.relative_to(ROOT).as_posix(),
        "fixture_git_blob": RUNNER_GIT_BLOB,
        "tests": len(tests),
        "executions": execution_count,
        "admitted": ADMITTED_EXECUTION_COUNT,
        "passed": passed,
        "failed": failed,
        "unsupported": len(unsupported_descriptions),
        "unsupported_descriptions": unsupported_descriptions,
        "admitted_surface_pass_claim": admitted_surface_pass,
        "full_fixture_pass_claim": False,
        "html_tokenizer_conformance_claim": False,
        "z7_status_change": False,
    }


def self_test(fixture: Path) -> None:
    tests = load_fixture(fixture)
    admitted, unsupported = fixture_partition(tests)
    require(
        len(admitted) == ADMITTED_EXECUTION_COUNT and
        len(unsupported) == UNSUPPORTED_EXECUTION_COUNT,
        "self-test denominator mismatch",
    )

    synthetic = "\n".join(
        [
            "TOKEN\tS\t68\t1\t2\t61\t62\t63\t64",
            "TOKEN\tM\t78",
            "TOKEN\tD\t68746d6c\t0\t\t0\t\t1",
            "TOKEN\tC\t7a",
            "TOKEN\tE\t68",
            "ERROR\t656f662d696e2d746167\t2\t3",
            "STATS\t1\t5\t1\t1\t1\t1",
        ]
    )
    tokens, errors, stats = parse_probe_output(synthetic)
    require(
        tokens
        == [
            ("S", "h", True, (("a", "b"), ("c", "d"))),
            ("M", "x"),
            ("D", "html", None, None, True),
            ("C", "z"),
            ("E", "h"),
        ],
        "self-test token wire parser mismatch",
    )
    require(errors == [("eof-in-tag", 2, 3)], "self-test error wire parser mismatch")
    require(stats == (1, 5, 1, 1, 1, 1), "self-test stats wire parser mismatch")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", type=Path)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--fixture", type=Path, default=DEFAULT_FIXTURE)
    parser.add_argument("--self-test", action="store_true")
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
        if args.self_test:
            self_test(fixture)
            print(
                json.dumps(
                    {
                        "schema": REPORT_SCHEMA,
                        "self_test_passed": True,
                        "admitted": ADMITTED_EXECUTION_COUNT,
                        "unsupported": UNSUPPORTED_EXECUTION_COUNT,
                        "html_tokenizer_conformance_claim": False,
                        "z7_status_change": False,
                    },
                    sort_keys=True,
                )
            )
            return 0

        require(args.probe is not None, "--probe is required unless --self-test is used")
        report = execute_fixture(args.probe.resolve(), fixture)
    except (RunnerError, VerificationError) as exc:
        print(f"Z7 html5lib test1 admitted runner failed: {exc}", file=sys.stderr)
        return 1

    print(json.dumps(report, sort_keys=True))
    return 0 if report["admitted_surface_pass_claim"] is True else 1


if __name__ == "__main__":
    raise SystemExit(main())
