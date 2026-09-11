#!/usr/bin/env python3
from __future__ import annotations

import argparse
import collections
import json
import re
import subprocess
import sys
from pathlib import Path
from typing import Any

from z7_html5lib_tokenizer_corpus_verify import VerificationError, verify_manifest

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MANIFEST = ROOT / "config/z7_html5lib_tokenizer_corpus.json"
REPORT_SCHEMA = "zevryon.z7.html5lib-tokenizer-full-corpus-census.v1"
EXPECTED_FILES = 14
EXPECTED_TESTS = 6810
EXPECTED_EXECUTIONS = 7036
MAX_SAMPLES = 25

STATE_MAP = {
    "Data state": "DATA",
    "PLAINTEXT state": "PLAINTEXT",
    "RCDATA state": "RCDATA",
    "RAWTEXT state": "RAWTEXT",
    "Script data state": "SCRIPT_DATA",
    "CDATA section state": "CDATA_SECTION",
}

DOUBLE_ESCAPE_RE = re.compile(r"\\u([0-9A-Fa-f]{4})")


class CensusError(ValueError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise CensusError(message)


def unescape_double_string(value: str) -> str:
    return DOUBLE_ESCAPE_RE.sub(lambda match: chr(int(match.group(1), 16)), value)


def unescape_double_value(value: Any) -> Any:
    if isinstance(value, str):
        return unescape_double_string(value)
    if isinstance(value, list):
        return [unescape_double_value(item) for item in value]
    if isinstance(value, dict):
        return {
            unescape_double_string(key): unescape_double_value(item)
            for key, item in value.items()
        }
    return value


def utf8_hex(value: str) -> str:
    try:
        return value.encode("utf-8").hex()
    except UnicodeEncodeError as exc:
        raise CensusError(f"cannot represent input as UTF-8 scalar bytes: {exc}") from exc


def decode_hex(value: str, label: str) -> str:
    try:
        return bytes.fromhex(value).decode("utf-8")
    except (ValueError, UnicodeError) as exc:
        raise CensusError(f"{label} contains invalid UTF-8 hex: {exc}") from exc


def parse_bool(value: str, label: str) -> bool:
    require(value in {"0", "1"}, f"{label} must be 0/1")
    return value == "1"


def expected_token(raw: Any, label: str) -> tuple[Any, ...]:
    require(isinstance(raw, list) and raw, f"{label} must be a non-empty array")
    kind = raw[0]
    require(isinstance(kind, str), f"{label} token kind must be string")
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
        require(isinstance(raw[1], str) and isinstance(raw[2], dict), f"{label} StartTag shape invalid")
        attrs: list[tuple[str, str]] = []
        for name, value in raw[2].items():
            require(
                isinstance(name, str) and isinstance(value, str),
                f"{label} StartTag attributes must be string-to-string",
            )
            attrs.append((name, value))
        self_closing = False
        if len(raw) == 4:
            require(raw[3] is True, f"{label} self-closing marker invalid")
            self_closing = True
        return ("S", raw[1], self_closing, tuple(sorted(attrs)))
    if kind == "DOCTYPE":
        require(len(raw) == 5, f"{label} DOCTYPE length invalid")
        name, public_id, system_id, correct = raw[1:]
        require(name is None or isinstance(name, str), f"{label} DOCTYPE name invalid")
        require(public_id is None or isinstance(public_id, str), f"{label} public id invalid")
        require(system_id is None or isinstance(system_id, str), f"{label} system id invalid")
        require(isinstance(correct, bool), f"{label} correctness invalid")
        return ("D", name, public_id, system_id, not correct)
    raise CensusError(f"{label} token kind unsupported: {kind!r}")


def expected_tokens(test: dict[str, Any], label: str) -> list[tuple[Any, ...]]:
    output = test.get("output")
    require(isinstance(output, list), f"{label} output must be array")
    return [expected_token(raw, f"{label} output[{index}]") for index, raw in enumerate(output)]


def expected_errors(test: dict[str, Any], label: str) -> list[tuple[str, int, int]]:
    raw_errors = test.get("errors", [])
    require(isinstance(raw_errors, list), f"{label} errors must be array")
    result: list[tuple[str, int, int]] = []
    for index, raw in enumerate(raw_errors):
        require(isinstance(raw, dict), f"{label} errors[{index}] must be object")
        code = raw.get("code")
        line = raw.get("line")
        col = raw.get("col")
        require(isinstance(code, str) and code, f"{label} errors[{index}] code invalid")
        require(isinstance(line, int) and not isinstance(line, bool) and line >= 1, f"{label} errors[{index}] line invalid")
        require(isinstance(col, int) and not isinstance(col, bool) and col >= 1, f"{label} errors[{index}] col invalid")
        result.append((code, line, col))
    return result


def parse_probe(stdout: str) -> tuple[list[tuple[Any, ...]], list[tuple[str, int, int]], tuple[int, ...]]:
    tokens: list[tuple[Any, ...]] = []
    errors: list[tuple[str, int, int]] = []
    stats: tuple[int, ...] | None = None
    for line_number, line in enumerate(stdout.splitlines(), start=1):
        fields = line.split("\t")
        require(fields and fields[0], f"probe line {line_number} empty")
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
                self_closing = parse_bool(fields[3], f"probe StartTag self-closing line {line_number}")
                try:
                    attr_count = int(fields[4], 10)
                except ValueError as exc:
                    raise CensusError(f"probe StartTag attribute count invalid on line {line_number}") from exc
                require(attr_count >= 0 and len(fields) == 5 + attr_count * 2, f"probe StartTag attributes malformed on line {line_number}")
                attrs: list[tuple[str, str]] = []
                for attr_index in range(attr_count):
                    base = 5 + attr_index * 2
                    attrs.append((decode_hex(fields[base], "probe attribute name"), decode_hex(fields[base + 1], "probe attribute value")))
                require(len(attrs) == len({name for name, _ in attrs}), f"probe duplicate published attribute on line {line_number}")
                tokens.append(("S", name, self_closing, tuple(sorted(attrs))))
                continue
            if kind == "D":
                require(len(fields) == 8, f"probe DOCTYPE line {line_number} malformed")
                name = decode_hex(fields[2], "probe DOCTYPE name")
                has_public = parse_bool(fields[3], "probe DOCTYPE public flag")
                public_id = decode_hex(fields[4], "probe DOCTYPE public id")
                has_system = parse_bool(fields[5], "probe DOCTYPE system flag")
                system_id = decode_hex(fields[6], "probe DOCTYPE system id")
                force_quirks = parse_bool(fields[7], "probe DOCTYPE force-quirks")
                require(has_public or public_id == "", "probe absent public id has payload")
                require(has_system or system_id == "", "probe absent system id has payload")
                tokens.append(("D", name, public_id if has_public else None, system_id if has_system else None, force_quirks))
                continue
            raise CensusError(f"probe token kind invalid on line {line_number}: {kind!r}")
        if fields[0] == "ERROR":
            require(len(fields) == 4, f"probe ERROR line {line_number} malformed")
            try:
                source_line = int(fields[2], 10)
                source_col = int(fields[3], 10)
            except ValueError as exc:
                raise CensusError(f"probe ERROR coordinates invalid on line {line_number}") from exc
            errors.append((decode_hex(fields[1], "probe ERROR code"), source_line, source_col))
            continue
        if fields[0] == "STATS":
            require(len(fields) == 7 and stats is None, f"probe STATS line {line_number} malformed")
            try:
                stats = tuple(int(value, 10) for value in fields[1:])
            except ValueError as exc:
                raise CensusError(f"probe STATS counters invalid on line {line_number}") from exc
            require(all(value >= 0 for value in stats), "probe STATS contains negative counter")
            continue
        if fields[0] == "FAIL":
            require(len(fields) == 2, f"probe FAIL line {line_number} malformed")
            raise CensusError(f"PROBE_FAIL:{decode_hex(fields[1], 'probe FAIL')}")
        raise CensusError(f"unknown probe record on line {line_number}: {fields[0]!r}")
    require(stats is not None, "probe output missing STATS")
    return tokens, errors, stats


def common_stats(input_text: str, tokens: list[tuple[Any, ...]], errors: list[tuple[str, int, int]]) -> tuple[int, ...]:
    chars = [token for token in tokens if token[0] == "C"]
    return (
        len(input_text.encode("utf-8")),
        len(tokens),
        len(chars),
        sum(len(str(token[1]).encode("utf-8")) for token in chars),
        sum(1 for token in tokens if token[0] == "E"),
        len(errors),
    )


def has_nullable_doctype_name(tokens: list[tuple[Any, ...]]) -> bool:
    return any(token[0] == "D" and token[1] is None for token in tokens)


def classify_pre_execution(
    upstream_path: str,
    state_name: str,
    input_text: str,
    last_start_tag: str,
    tokens: list[tuple[Any, ...]],
) -> str | None:
    if upstream_path == "tokenizer/xmlViolation.test":
        return "xml-violation-infoset-coercion"
    if state_name not in STATE_MAP:
        return f"initial-state:{state_name}"
    if has_nullable_doctype_name(tokens):
        return "probe-wire-null-doctype-name"
    try:
        input_text.encode("utf-8")
        last_start_tag.encode("utf-8")
        for token in tokens:
            for item in token:
                if isinstance(item, str):
                    item.encode("utf-8")
    except UnicodeEncodeError:
        return "non-utf8-scalar-test-data"
    # The html5lib CDATA initial-state authority intentionally preserves raw
    # NUL as Character data. Do not route that state through the generic
    # Data/text-state NUL preprocessing debt bucket.
    if "\x00" in input_text and state_name != "CDATA section state":
        return "input-preprocessing-nul"
    return None


def execute_one(
    probe: Path,
    state: str,
    last_start_tag: str,
    input_text: str,
    expected_token_stream: list[tuple[Any, ...]],
    expected_error_stream: list[tuple[str, int, int]],
) -> tuple[str, str]:
    command = [str(probe), state, utf8_hex(last_start_tag), utf8_hex(input_text)]
    try:
        completed = subprocess.run(command, check=False, capture_output=True, text=True, timeout=5)
    except subprocess.TimeoutExpired:
        return "failed", "probe-timeout"
    except OSError as exc:
        raise CensusError(f"cannot execute production tokenizer probe: {exc}") from exc

    if completed.returncode == 2:
        detail = completed.stdout.strip() or completed.stderr.strip() or "probe returned fail without detail"
        if detail.startswith("FAIL\t"):
            try:
                detail = decode_hex(detail.split("\t", 1)[1], "probe FAIL")
            except CensusError:
                pass
        return "unsupported", f"production-fail-closed:{detail[:160]}"
    if completed.returncode != 0:
        return "failed", f"probe-return-code:{completed.returncode}"
    if completed.stderr.strip():
        return "failed", "probe-unexpected-stderr"

    try:
        actual_tokens, actual_errors, actual_stats = parse_probe(completed.stdout)
    except CensusError as exc:
        return "failed", f"probe-wire:{exc}"

    if actual_tokens != expected_token_stream:
        return "failed", "token-stream-mismatch"
    if actual_errors != expected_error_stream:
        return "failed", "parse-error-stream-mismatch"
    try:
        expected_stats = common_stats(input_text, expected_token_stream, expected_error_stream)
    except UnicodeEncodeError:
        return "unsupported", "non-utf8-scalar-expected-output"
    if actual_stats != expected_stats:
        return "failed", "common-stats-mismatch"
    return "passed", "exact-match"


def add_sample(samples: list[dict[str, Any]], item: dict[str, Any]) -> None:
    if len(samples) < MAX_SAMPLES:
        samples.append(item)


def execute_census(probe: Path, manifest_path: Path) -> dict[str, Any]:
    require(probe.is_file(), f"tokenizer probe missing: {probe}")
    try:
        provenance = verify_manifest(manifest_path, ROOT)
    except VerificationError as exc:
        raise CensusError(str(exc)) from exc
    require(provenance.get("provenance_gate_passed") is True, "corpus provenance gate failed")
    require(provenance.get("files_verified") == EXPECTED_FILES, "corpus file denominator drifted")
    require(provenance.get("tests_verified") == EXPECTED_TESTS, "corpus test denominator drifted")
    require(provenance.get("executions_verified") == EXPECTED_EXECUTIONS, "corpus execution denominator drifted")
    require(provenance.get("conformance_claim") is False, "provenance unexpectedly claims conformance")

    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    files = manifest.get("files")
    require(isinstance(files, list) and len(files) == EXPECTED_FILES, "manifest files invalid")

    totals = collections.Counter()
    reason_counts: collections.Counter[str] = collections.Counter()
    state_counts: collections.Counter[str] = collections.Counter()
    file_reports: dict[str, dict[str, int]] = {}
    failure_samples: list[dict[str, Any]] = []
    unsupported_samples: list[dict[str, Any]] = []

    for entry in files:
        require(isinstance(entry, dict), "manifest file entry invalid")
        upstream_path = entry.get("upstream_path")
        vendored_path = entry.get("vendored_path")
        test_array_key = entry.get("test_array_key")
        require(isinstance(upstream_path, str), "manifest upstream_path invalid")
        require(isinstance(vendored_path, str), "manifest vendored_path invalid")
        require(isinstance(test_array_key, str), "manifest test_array_key invalid")
        fixture_path = ROOT / vendored_path
        payload = json.loads(fixture_path.read_text(encoding="utf-8"))
        tests = payload.get(test_array_key)
        require(isinstance(tests, list), f"{upstream_path}: pinned test array missing")
        per_file = collections.Counter()

        for test_index, raw_test in enumerate(tests):
            require(isinstance(raw_test, dict), f"{upstream_path} test[{test_index}] invalid")
            description = raw_test.get("description")
            require(isinstance(description, str) and description, f"{upstream_path} test[{test_index}] description invalid")
            test = unescape_double_value(raw_test) if raw_test.get("doubleEscaped", False) is True else raw_test
            input_text = test.get("input")
            last_start_tag = test.get("lastStartTag", "")
            states = test.get("initialStates", ["Data state"])
            require(isinstance(input_text, str), f"{upstream_path} test[{test_index}] input invalid")
            require(isinstance(last_start_tag, str), f"{upstream_path} test[{test_index}] lastStartTag invalid")
            require(isinstance(states, list) and states, f"{upstream_path} test[{test_index}] states invalid")
            label_base = f"{upstream_path} test[{test_index}] {description}"
            token_stream = expected_tokens(test, label_base)
            error_stream = expected_errors(test, label_base)

            for state_name in states:
                require(isinstance(state_name, str), f"{label_base}: state invalid")
                totals["executions"] += 1
                per_file["executions"] += 1
                state_counts[state_name] += 1
                unsupported_reason = classify_pre_execution(
                    upstream_path,
                    state_name,
                    input_text,
                    last_start_tag,
                    token_stream,
                )
                label = f"{label_base} [{state_name}]"
                if unsupported_reason is not None:
                    totals["unsupported"] += 1
                    per_file["unsupported"] += 1
                    reason_counts[f"unsupported:{unsupported_reason}"] += 1
                    add_sample(unsupported_samples, {"label": label, "reason": unsupported_reason})
                    continue

                status, reason = execute_one(
                    probe,
                    STATE_MAP[state_name],
                    last_start_tag,
                    input_text,
                    token_stream,
                    error_stream,
                )
                totals[status] += 1
                per_file[status] += 1
                reason_counts[f"{status}:{reason}"] += 1
                if status == "failed":
                    add_sample(failure_samples, {"label": label, "reason": reason})
                elif status == "unsupported":
                    add_sample(unsupported_samples, {"label": label, "reason": reason})

        file_reports[upstream_path] = {
            "executions": per_file["executions"],
            "passed": per_file["passed"],
            "failed": per_file["failed"],
            "unsupported": per_file["unsupported"],
        }

    require(totals["executions"] == EXPECTED_EXECUTIONS, "executed denominator drifted")
    require(
        totals["passed"] + totals["failed"] + totals["unsupported"] == EXPECTED_EXECUTIONS,
        "census accounting drifted",
    )

    return {
        "schema": REPORT_SCHEMA,
        "authority": "diagnostic-census-only",
        "files": EXPECTED_FILES,
        "tests": EXPECTED_TESTS,
        "executions": EXPECTED_EXECUTIONS,
        "passed": totals["passed"],
        "failed": totals["failed"],
        "unsupported": totals["unsupported"],
        "exact_execution_accounting": True,
        "file_reports": file_reports,
        "state_counts": dict(sorted(state_counts.items())),
        "reason_counts": dict(sorted(reason_counts.items())),
        "failure_samples": failure_samples,
        "unsupported_samples": unsupported_samples,
        "full_corpus_pass_claim": totals["passed"] == EXPECTED_EXECUTIONS and totals["failed"] == 0 and totals["unsupported"] == 0,
        "html_tokenizer_conformance_claim": False,
        "z7_status_change": False,
    }


def self_test() -> None:
    require(unescape_double_string(r"A\u0042C") == "ABC", "double escape ASCII decode")
    require(unescape_double_string(r"\u00AC") == "¬", "double escape Unicode decode")
    token = expected_token(["DOCTYPE", None, None, None, False], "self-test")
    require(token == ("D", None, None, None, True), "nullable DOCTYPE expectation")
    require(
        classify_pre_execution("tokenizer/test2.test", "CDATA section state", "x", "", []) is None,
        "CDATA admitted classification",
    )
    require(
        classify_pre_execution("tokenizer/domjs.test", "CDATA section state", "\x00]]>", "", []) is None,
        "CDATA raw NUL bypasses generic preprocessing bucket",
    )
    require(
        classify_pre_execution("tokenizer/test2.test", "Data state", "a\x00b", "", []) == "input-preprocessing-nul",
        "NUL preprocessing classification",
    )
    require(
        classify_pre_execution("tokenizer/xmlViolation.test", "Data state", "x", "", []) == "xml-violation-infoset-coercion",
        "xmlViolation classification",
    )


def main() -> int:
    parser = argparse.ArgumentParser(description="Execute an honest diagnostic census over the complete pinned Z7 html5lib tokenizer corpus")
    parser.add_argument("--probe", type=Path)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    try:
        if args.self_test:
            self_test()
            print(json.dumps({"schema": REPORT_SCHEMA, "self_test_passed": True}, sort_keys=True))
            return 0
        require(args.probe is not None, "--probe is required")
        require(args.manifest.resolve() == DEFAULT_MANIFEST.resolve(), "census only accepts canonical manifest path")
        report = execute_census(args.probe.resolve(), args.manifest.resolve())
    except (CensusError, OSError, UnicodeError, json.JSONDecodeError) as exc:
        print(f"Z7 full tokenizer corpus census failed: {exc}", file=sys.stderr)
        return 1
    print(json.dumps(report, sort_keys=True, separators=(",", ":"), ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
