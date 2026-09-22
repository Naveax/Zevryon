from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MANIFEST = ROOT / "config" / "z3_css_parser_corpus_v1.json"
SCHEMA = "zevryon.z3-css-parser-corpus.v1"
AUTHORITY = "z3-css-parser-conformance-v1"
REPORT_SCHEMA = "zevryon.z3-css-parser-runner.v1"
UPSTREAM_REPOSITORY = "web-platform-tests/wpt"
UPSTREAM_COMMIT = "15df54d4459b78242d32ae36f9c094a96972bedc"
FROZEN_SOURCES = {
    "input-preprocessing": (
        "css/css-syntax/input-preprocessing.html",
        "9ef9a730820d85a015b51cb230aa09f397a78400",
    ),
    "declaration-whitespace": (
        "css/css-syntax/declarations-trim-whitespace.html",
        "a7d69d149e7bbf854efc2ef19af132af372ff521",
    ),
    "missing-semicolon": (
        "css/css-syntax/missing-semicolon.html",
        "d8e70e631591c05d2025d1800b1201ce80550fed",
    ),
    "charset": (
        "css/css-syntax/charset-is-not-a-rule.html",
        "ff8d3298b6216f7cedb29939554a30c65eac4bf0",
    ),
    "at-rule-declaration-list": (
        "css/css-syntax/at-rule-in-declaration-list.html",
        "f40975d27eadf436634491b4a158cee4f9c268ef",
    ),
}
EXPECTED_CASES = 33
STAT_NAMES = [
    "input_bytes",
    "preprocessed_input_bytes",
    "null_replacements",
    "newline_normalizations",
    "invalid_utf8_replacements",
    "rules",
    "declarations",
    "important_declarations",
    "comments",
    "maximum_nesting_depth",
    "output_text_bytes",
    "at_rules",
    "dropped_charset_rules",
    "recovered_invalid_declarations",
]


class RunnerError(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RunnerError(message)


def decode_hex(value: str) -> str:
    try:
        return bytes.fromhex(value).decode("utf-8")
    except (ValueError, UnicodeDecodeError) as exc:
        raise RunnerError(f"invalid probe hex payload: {value!r}") from exc


def verify_manifest(payload: dict[str, Any]) -> list[dict[str, Any]]:
    require(payload.get("schema") == SCHEMA, "parser corpus schema mismatch")
    require(payload.get("authority") == AUTHORITY, "parser corpus authority mismatch")

    upstream = payload.get("upstream")
    require(isinstance(upstream, dict), "parser corpus upstream must be an object")
    require(
        upstream.get("repository") == UPSTREAM_REPOSITORY,
        "parser corpus upstream repository mismatch",
    )
    require(
        upstream.get("commit") == UPSTREAM_COMMIT,
        "parser corpus upstream commit mismatch",
    )
    sources = upstream.get("sources")
    require(isinstance(sources, dict), "parser corpus sources must be an object")
    require(set(sources) == set(FROZEN_SOURCES), "parser corpus source set mismatch")
    for name, (path, blob_sha) in FROZEN_SOURCES.items():
        source = sources.get(name)
        require(isinstance(source, dict), f"source {name} must be an object")
        require(source.get("path") == path, f"source {name} path mismatch")
        require(source.get("blob_sha") == blob_sha, f"source {name} blob mismatch")

    cases = payload.get("cases")
    require(isinstance(cases, list), "parser corpus cases must be an array")
    require(payload.get("case_count") == EXPECTED_CASES, "parser corpus case_count mismatch")
    require(len(cases) == EXPECTED_CASES, "parser corpus denominator mismatch")

    ids: set[str] = set()
    for index, case in enumerate(cases):
        require(isinstance(case, dict), f"case {index} must be an object")
        case_id = case.get("id")
        require(isinstance(case_id, str) and case_id, f"case {index} id missing")
        require(case_id not in ids, f"duplicate case id {case_id}")
        ids.add(case_id)

        source = case.get("source")
        require(
            source == "local" or source in FROZEN_SOURCES,
            f"case {case_id} has unknown provenance source",
        )
        has_utf8 = "input_utf8" in case
        has_bytes = "input_bytes" in case
        require(has_utf8 != has_bytes, f"case {case_id} must define exactly one input form")
        if has_utf8:
            require(isinstance(case["input_utf8"], str), f"case {case_id} input_utf8 must be string")
        else:
            values = case["input_bytes"]
            require(isinstance(values, list) and values, f"case {case_id} input_bytes must be nonempty")
            require(
                all(isinstance(value, int) and 0 <= value <= 255 for value in values),
                f"case {case_id} input_bytes must contain bytes",
            )

        expect = case.get("expect")
        require(isinstance(expect, dict), f"case {case_id} expect must be object")
        is_error = "error_kind" in expect
        is_success = "rules" in expect
        require(is_error != is_success, f"case {case_id} expectation must be success or error")
        if is_success:
            require(isinstance(expect.get("rules"), list), f"case {case_id} rules must be array")
            require(isinstance(expect.get("at_rules"), list), f"case {case_id} at_rules must be array")
            stats = expect.get("stats", {})
            require(isinstance(stats, dict), f"case {case_id} stats must be object")
            require(set(stats).issubset(STAT_NAMES), f"case {case_id} has unknown stat key")
        else:
            require(
                isinstance(expect.get("error_kind"), str) and expect["error_kind"],
                f"case {case_id} error_kind missing",
            )
    return cases


def parse_probe(stdout: bytes) -> dict[str, Any]:
    try:
        lines = stdout.decode("utf-8").splitlines()
    except UnicodeDecodeError as exc:
        raise RunnerError("probe stdout is not UTF-8") from exc
    require(lines, "probe returned empty output")

    first = lines[0].split("\t")
    require(len(first) == 2 and first[0] == "STATUS", "probe STATUS line missing")
    status = first[1]
    if status == "ERROR":
        require(len(lines) == 2, "error probe output must contain exactly two lines")
        parts = lines[1].split("\t")
        require(len(parts) == 3 and parts[0] == "ERROR", "probe ERROR line malformed")
        return {
            "status": "ERROR",
            "error_kind": parts[1],
            "byte_offset": int(parts[2]),
        }

    require(status == "OK", f"unknown probe status {status!r}")
    rules_raw: list[dict[str, Any]] = []
    declarations: list[dict[str, Any]] = []
    at_rules: list[dict[str, Any]] = []
    stats: dict[str, int] | None = None
    rule_count: int | None = None
    decl_count: int | None = None
    at_rule_count: int | None = None

    for line in lines[1:]:
        parts = line.split("\t")
        tag = parts[0]
        if tag == "RULE_COUNT":
            require(len(parts) == 2, "RULE_COUNT line malformed")
            rule_count = int(parts[1])
        elif tag == "R":
            require(len(parts) == 5, "R line malformed")
            rules_raw.append(
                {
                    "index": int(parts[1]),
                    "selector": decode_hex(parts[2]),
                    "declaration_offset": int(parts[3]),
                    "declaration_count": int(parts[4]),
                }
            )
        elif tag == "DECL_COUNT":
            require(len(parts) == 2, "DECL_COUNT line malformed")
            decl_count = int(parts[1])
        elif tag == "D":
            require(len(parts) == 5, "D line malformed")
            declarations.append(
                {
                    "index": int(parts[1]),
                    "property": decode_hex(parts[2]),
                    "value": decode_hex(parts[3]),
                    "important": parts[4] == "1",
                }
            )
        elif tag == "ATRULE_COUNT":
            require(len(parts) == 2, "ATRULE_COUNT line malformed")
            at_rule_count = int(parts[1])
        elif tag == "A":
            require(len(parts) == 8, "A line malformed")
            at_rules.append(
                {
                    "index": int(parts[1]),
                    "context": parts[2],
                    "owner": int(parts[3]),
                    "has_block": parts[4] == "1",
                    "name": decode_hex(parts[5]),
                    "prelude": decode_hex(parts[6]),
                    "block": decode_hex(parts[7]),
                }
            )
        elif tag == "STATS":
            require(len(parts) == 1 + len(STAT_NAMES), "STATS line malformed")
            stats = {
                name: int(value)
                for name, value in zip(STAT_NAMES, parts[1:], strict=True)
            }
        else:
            raise RunnerError(f"unknown probe line tag {tag!r}")

    require(rule_count == len(rules_raw), "probe rule count mismatch")
    require(decl_count == len(declarations), "probe declaration count mismatch")
    require(at_rule_count == len(at_rules), "probe at-rule count mismatch")
    require(stats is not None, "probe STATS line missing")

    for expected_index, declaration in enumerate(declarations):
        require(
            declaration["index"] == expected_index,
            "probe declaration indices are not contiguous",
        )

    rules: list[dict[str, Any]] = []
    for expected_index, raw in enumerate(rules_raw):
        require(raw["index"] == expected_index, "probe rule indices are not contiguous")
        begin = raw["declaration_offset"]
        end = begin + raw["declaration_count"]
        require(0 <= begin <= end <= len(declarations), "probe declaration range is invalid")
        rules.append(
            {
                "selector": raw["selector"],
                "declarations": [
                    {
                        "property": item["property"],
                        "value": item["value"],
                        "important": item["important"],
                    }
                    for item in declarations[begin:end]
                ],
            }
        )

    normalized_at_rules: list[dict[str, Any]] = []
    for expected_index, rule in enumerate(at_rules):
        require(rule["index"] == expected_index, "probe at-rule indices are not contiguous")
        normalized_at_rules.append(
            {
                "context": rule["context"],
                "owner": rule["owner"],
                "has_block": rule["has_block"],
                "name": rule["name"],
                "prelude": rule["prelude"],
                "block": rule["block"],
            }
        )

    return {
        "status": "OK",
        "rules": rules,
        "at_rules": normalized_at_rules,
        "stats": stats,
    }


def case_input(case: dict[str, Any]) -> bytes:
    if "input_utf8" in case:
        return case["input_utf8"].encode("utf-8")
    return bytes(case["input_bytes"])


def compare_case(case: dict[str, Any], actual: dict[str, Any]) -> str | None:
    expect = case["expect"]
    if "error_kind" in expect:
        if actual.get("status") != "ERROR":
            return f"expected error {expect['error_kind']}, got success"
        if actual.get("error_kind") != expect["error_kind"]:
            return (
                f"expected error {expect['error_kind']}, "
                f"got {actual.get('error_kind')}"
            )
        return None

    if actual.get("status") != "OK":
        return f"expected success, got error {actual.get('error_kind')}"
    if actual.get("rules") != expect["rules"]:
        return (
            "rule/declaration mismatch\n"
            f"expected={json.dumps(expect['rules'], ensure_ascii=False)}\n"
            f"actual={json.dumps(actual.get('rules'), ensure_ascii=False)}"
        )
    if actual.get("at_rules") != expect["at_rules"]:
        return (
            "at-rule mismatch\n"
            f"expected={json.dumps(expect['at_rules'], ensure_ascii=False)}\n"
            f"actual={json.dumps(actual.get('at_rules'), ensure_ascii=False)}"
        )
    for name, value in expect.get("stats", {}).items():
        actual_value = actual["stats"].get(name)
        if actual_value != value:
            return f"stat {name} expected {value}, got {actual_value}"
    return None


def run(probe: Path, manifest: Path) -> dict[str, Any]:
    payload = json.loads(manifest.read_text(encoding="utf-8"))
    cases = verify_manifest(payload)

    failures: list[dict[str, str]] = []
    passed = 0
    for case in cases:
        completed = subprocess.run(
            [str(probe)],
            input=case_input(case),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
            timeout=10,
        )
        if completed.returncode != 0:
            failures.append(
                {
                    "id": case["id"],
                    "reason": (
                        f"probe exited {completed.returncode}: "
                        f"{completed.stderr.decode('utf-8', errors='replace')}"
                    ),
                }
            )
            continue
        try:
            actual = parse_probe(completed.stdout)
            mismatch = compare_case(case, actual)
        except RunnerError as exc:
            mismatch = str(exc)
        if mismatch is None:
            passed += 1
        else:
            failures.append({"id": case["id"], "reason": mismatch})

    report = {
        "schema": REPORT_SCHEMA,
        "authority": AUTHORITY,
        "configured_cases": len(cases),
        "passed": passed,
        "failed": len(failures),
        "unsupported": 0,
        "css_parser_conformance_claim": (
            passed == len(cases) and not failures
        ),
        "failures": failures,
    }
    return report


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run the complete frozen Z3 CSS parser authority corpus"
    )
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    args = parser.parse_args()

    try:
        report = run(args.probe, args.manifest)
    except (OSError, json.JSONDecodeError, RunnerError, subprocess.TimeoutExpired) as exc:
        print(f"Z3 CSS parser authority runner error: {exc}", file=sys.stderr)
        return 2

    print(json.dumps(report, ensure_ascii=False, sort_keys=True))
    if not report["css_parser_conformance_claim"]:
        for failure in report["failures"]:
            print(
                f"FAILED {failure['id']}: {failure['reason']}",
                file=sys.stderr,
            )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
