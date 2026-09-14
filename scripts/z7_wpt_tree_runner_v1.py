#!/usr/bin/env python3
from __future__ import annotations

import argparse
import collections
import json
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from z7_wpt_tree_corpus_verify import VerificationError, verify_manifest

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MANIFEST = ROOT / "config/z7_wpt_tree_corpus.json"
REPORT_SCHEMA = "zevryon.z7.wpt-tree-runner.v1"
MAX_SAMPLES = 20


class RunnerError(ValueError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RunnerError(message)


@dataclass(frozen=True)
class TreeCase:
    source_path: str
    index: int
    data: str
    errors: tuple[str, ...]
    document: tuple[str, ...]
    fragment_context: str | None
    scripting_modes: tuple[bool, ...]


def parse_fixture(path: Path, source_path: str) -> list[TreeCase]:
    try:
        text = path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as exc:
        raise RunnerError(f"cannot read WPT tree fixture {path}: {exc}") from exc
    require(text.endswith("\n") and "\r" not in text, f"{source_path}: fixture framing drifted")
    blocks = text[:-1].split("\n\n")
    cases: list[TreeCase] = []
    for index, block in enumerate(blocks):
        lines = block.split("\n")
        require(lines and lines[0] == "#data", f"{source_path}[{index}] missing #data")
        try:
            errors_index = lines.index("#errors", 1)
        except ValueError as exc:
            raise RunnerError(f"{source_path}[{index}] missing #errors") from exc
        data = "\n".join(lines[1:errors_index])
        cursor = errors_index + 1
        errors: list[str] = []
        while cursor < len(lines) and not lines[cursor].startswith("#"):
            require(lines[cursor] != "", f"{source_path}[{index}] empty error record")
            errors.append(lines[cursor])
            cursor += 1
        if cursor < len(lines) and lines[cursor] == "#new-errors":
            cursor += 1
            while cursor < len(lines) and not lines[cursor].startswith("#"):
                require(lines[cursor] != "", f"{source_path}[{index}] empty new-error record")
                errors.append(lines[cursor])
                cursor += 1

        fragment_context: str | None = None
        if cursor < len(lines) and lines[cursor] == "#document-fragment":
            cursor += 1
            require(cursor < len(lines), f"{source_path}[{index}] missing fragment context")
            fragment_context = lines[cursor]
            cursor += 1

        scripting_modes: tuple[bool, ...] = (False, True)
        if cursor < len(lines) and lines[cursor] == "#script-off":
            scripting_modes = (False,)
            cursor += 1
        elif cursor < len(lines) and lines[cursor] == "#script-on":
            scripting_modes = (True,)
            cursor += 1

        require(cursor < len(lines) and lines[cursor] == "#document",
                f"{source_path}[{index}] missing #document")
        document = tuple(lines[cursor + 1 :])
        require(document and all(line.startswith("| ") for line in document),
                f"{source_path}[{index}] malformed expected tree")
        cases.append(TreeCase(
            source_path=source_path,
            index=index,
            data=data,
            errors=tuple(errors),
            document=document,
            fragment_context=fragment_context,
            scripting_modes=scripting_modes,
        ))
    return cases


def decode_hex(value: str, label: str) -> str:
    try:
        return bytes.fromhex(value).decode("utf-8")
    except (ValueError, UnicodeError) as exc:
        raise RunnerError(f"{label}: invalid UTF-8 hex: {exc}") from exc


def parse_probe_output(stdout: str, expected_mode: bool) -> tuple[tuple[str, ...], dict[str, bool]]:
    mode: bool | None = None
    capabilities: dict[str, bool] | None = None
    tree: list[str] = []
    saw_stats = False
    for line_number, line in enumerate(stdout.splitlines(), start=1):
        fields = line.split("\t")
        require(fields and fields[0], f"probe line {line_number} is empty")
        if fields[0] == "MODE":
            require(len(fields) == 2 and fields[1] in {"0", "1"} and mode is None,
                    f"probe MODE line {line_number} malformed")
            mode = fields[1] == "1"
        elif fields[0] == "CAPS":
            require(capabilities is None and len(fields) >= 2,
                    f"probe CAPS line {line_number} malformed")
            capabilities = {}
            for field in fields[1:]:
                key, separator, value = field.partition("=")
                require(separator == "=" and key and value in {"0", "1"},
                        f"probe CAPS field malformed: {field!r}")
                capabilities[key] = value == "1"
        elif fields[0] == "TREE":
            require(len(fields) == 2, f"probe TREE line {line_number} malformed")
            tree.append(decode_hex(fields[1], f"probe TREE line {line_number}"))
        elif fields[0] == "STATS":
            require(len(fields) == 3 and not saw_stats,
                    f"probe STATS line {line_number} malformed")
            try:
                node_count = int(fields[1], 10)
                line_count = int(fields[2], 10)
            except ValueError as exc:
                raise RunnerError("probe STATS contains non-integer values") from exc
            require(node_count >= 1 and line_count == len(tree), "probe STATS disagrees with tree output")
            saw_stats = True
        else:
            raise RunnerError(f"unknown probe record: {fields[0]!r}")
    require(mode is expected_mode, "probe scripting-mode echo mismatch")
    require(capabilities is not None, "probe output missing CAPS")
    require(saw_stats, "probe output missing STATS")
    return tuple(tree), capabilities


def parse_unsupported(stdout: str) -> str:
    lines = stdout.splitlines()
    require(len(lines) == 1, "unsupported probe output must contain one record")
    fields = lines[0].split("\t")
    require(len(fields) == 2 and fields[0] == "UNSUPPORTED", "unsupported probe record malformed")
    return decode_hex(fields[1], "unsupported probe reason")


def execute_probe(probe: Path, case: TreeCase, scripting: bool) -> tuple[str, str, tuple[str, ...] | None, dict[str, bool] | None]:
    command = [str(probe), "1" if scripting else "0", case.data.encode("utf-8").hex()]
    try:
        completed = subprocess.run(command, check=False, capture_output=True, text=True, timeout=10)
    except subprocess.TimeoutExpired:
        return "failed", "probe-timeout", None, None
    except OSError as exc:
        raise RunnerError(f"cannot execute tree probe: {exc}") from exc
    if completed.stderr.strip():
        return "failed", "probe-unexpected-stderr", None, None
    if completed.returncode == 2:
        try:
            reason = parse_unsupported(completed.stdout)
        except RunnerError as exc:
            return "failed", f"probe-wire:{exc}", None, None
        return "unsupported", f"production-fail-closed:{reason[:180]}", None, None
    if completed.returncode != 0:
        return "failed", f"probe-return-code:{completed.returncode}", None, None
    try:
        tree, capabilities = parse_probe_output(completed.stdout, scripting)
    except RunnerError as exc:
        return "failed", f"probe-wire:{exc}", None, None
    return "ok", "", tree, capabilities


def known_capability_boundary(case: TreeCase, capabilities: dict[str, bool]) -> str | None:
    if case.fragment_context is not None and not capabilities.get("fragments", False):
        return "document-fragment-context-not-exposed"
    if case.errors and not capabilities.get("parse-errors", False):
        return "parse-error-stream-authority-not-exposed"
    if any(line.lstrip().startswith('| "') for line in case.document) and not capabilities.get("text-nodes", False):
        return "text-node-materialization-not-exposed"
    if any("<!--" in line for line in case.document) and not capabilities.get("comments", False):
        return "comment-node-materialization-not-exposed"
    if any("math " in line or "svg " in line for line in case.document) and not capabilities.get("namespaces", False):
        return "namespace-tree-authority-not-exposed"
    return None


def add_sample(samples: list[dict[str, Any]], value: dict[str, Any]) -> None:
    if len(samples) < MAX_SAMPLES:
        samples.append(value)


def run(probe: Path, manifest_path: Path) -> dict[str, Any]:
    require(probe.is_file(), f"tree probe missing: {probe}")
    try:
        provenance = verify_manifest(manifest_path, ROOT)
    except VerificationError as exc:
        raise RunnerError(str(exc)) from exc
    require(provenance.get("provenance_gate_passed") is True, "WPT tree provenance gate failed")
    require(provenance.get("conformance_claim") is False, "WPT corpus unexpectedly claims conformance")

    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    totals: collections.Counter[str] = collections.Counter()
    reasons: collections.Counter[str] = collections.Counter()
    samples: list[dict[str, Any]] = []
    tests = 0
    for entry in manifest["files"]:
        cases = parse_fixture(ROOT / entry["vendored_path"], entry["upstream_path"])
        tests += len(cases)
        for case in cases:
            for scripting in case.scripting_modes:
                totals["executions"] += 1
                if case.fragment_context is not None:
                    totals["unsupported"] += 1
                    reasons["document-fragment-context-not-exposed"] += 1
                    add_sample(samples, {
                        "file": case.source_path,
                        "test_index": case.index,
                        "scripting": scripting,
                        "status": "unsupported",
                        "reason": "document-fragment-context-not-exposed",
                    })
                    continue
                status, reason, actual_tree, capabilities = execute_probe(probe, case, scripting)
                if status != "ok":
                    totals[status] += 1
                    reasons[reason] += 1
                    add_sample(samples, {
                        "file": case.source_path,
                        "test_index": case.index,
                        "scripting": scripting,
                        "status": status,
                        "reason": reason,
                    })
                    continue
                assert actual_tree is not None and capabilities is not None
                boundary = known_capability_boundary(case, capabilities)
                if boundary is not None:
                    totals["unsupported"] += 1
                    reasons[boundary] += 1
                    add_sample(samples, {
                        "file": case.source_path,
                        "test_index": case.index,
                        "scripting": scripting,
                        "status": "unsupported",
                        "reason": boundary,
                    })
                    continue
                if actual_tree != case.document:
                    totals["failed"] += 1
                    reasons["tree-dump-mismatch"] += 1
                    add_sample(samples, {
                        "file": case.source_path,
                        "test_index": case.index,
                        "scripting": scripting,
                        "status": "failed",
                        "reason": "tree-dump-mismatch",
                        "expected": list(case.document[:8]),
                        "actual": list(actual_tree[:8]),
                    })
                    continue
                totals["passed"] += 1
                reasons["exact-tree-match"] += 1

    require(totals["executions"] == provenance["executions_verified"], "runner execution denominator drifted")
    return {
        "schema": REPORT_SCHEMA,
        "authority": "z7-wpt-tree-production-runner-v1",
        "upstream_commit": provenance["upstream_commit"],
        "files": provenance["files_verified"],
        "tests": tests,
        "executions": totals["executions"],
        "passed": totals["passed"],
        "failed": totals["failed"],
        "unsupported": totals["unsupported"],
        "reason_counts": dict(sorted(reasons.items())),
        "samples": samples,
        "production_probe": str(probe),
        "tree_builder_conformance_claim": False,
        "z7_status_change": False,
    }


def self_test(manifest_path: Path) -> dict[str, Any]:
    try:
        provenance = verify_manifest(manifest_path, ROOT)
    except VerificationError as exc:
        raise RunnerError(str(exc)) from exc
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    cases: list[TreeCase] = []
    for entry in manifest["files"]:
        cases.extend(parse_fixture(ROOT / entry["vendored_path"], entry["upstream_path"]))
    require(len(cases) == provenance["tests_verified"], "self-test tree-test denominator drifted")
    require(sum(len(case.scripting_modes) for case in cases) == provenance["executions_verified"],
            "self-test execution denominator drifted")
    require(sum(len(case.errors) for case in cases) == 17, "self-test expected-error denominator drifted")
    synthetic = "MODE\t0\nCAPS\tparse-errors=0\tfragments=0\ttext-nodes=0\tcomments=0\tnamespaces=0\nTREE\t7c203c68746d6c3e\nSTATS\t2\t1\n"
    tree, caps = parse_probe_output(synthetic, False)
    require(tree == ("| <html>",), "self-test probe tree decode drifted")
    require(caps.get("parse-errors") is False, "self-test capability decode drifted")
    require(known_capability_boundary(cases[0], caps) == "parse-error-stream-authority-not-exposed",
            "self-test capability boundary drifted")
    return {
        "schema": "zevryon.z7.wpt-tree-runner-self-test.v1",
        "tests_verified": len(cases),
        "executions_verified": sum(len(case.scripting_modes) for case in cases),
        "self_test_passed": True,
        "conformance_claim": False,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Run frozen WPT tree cases through Zevryon production tree adapter")
    parser.add_argument("--probe", type=Path)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    try:
        if args.self_test:
            report = self_test(args.manifest)
        else:
            require(args.probe is not None, "--probe is required unless --self-test is used")
            report = run(args.probe, args.manifest)
    except RunnerError as exc:
        print(f"Z7 WPT tree runner error: {exc}", file=sys.stderr)
        return 2
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
