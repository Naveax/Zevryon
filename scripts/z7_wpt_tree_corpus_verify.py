#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import sys
import tempfile
from pathlib import Path
from typing import Any

SCHEMA = "zevryon.z7.wpt-tree-corpus.v1"
AUTHORITY = "z7-wpt-tree-corpus-provenance-v1"
REPORT_SCHEMA = "zevryon.z7.wpt-tree-corpus-verification.v1"
UPSTREAM_REPOSITORY = "web-platform-tests/wpt"
PINNED_UPSTREAM_COMMIT = "fadb01bc53cd4a9fac9352c977df1787425b856e"
DEFAULT_MANIFEST = Path("config/z7_wpt_tree_corpus.json")

PINNED_LICENSE = {
    "vendored_path": "third_party/wpt/LICENSE.md",
    "git_blob": "39c46d03ac2988226f949ee7ab3c7347d5481bd8",
    "size_bytes": 1500,
    "sha256": "5fac07febb0e2a97fb0d7b0def149ec08b642e1ba4b9c345283ab1cbd2af6570",
}
PINNED_FIXTURES = {
    "html/syntax/parsing/resources/adoption02.dat": {
        "vendored_path": "tests/fixtures/wpt-tree-construction/adoption02.dat",
        "git_blob": "880cb505a660efdcacd287d50702d6b68a1c94d7",
        "size_bytes": 1343,
        "sha256": "e091e6976f861ae616fe56c527a78e7247ee7562bcafec996d4e4bda657bd9b7",
        "test_count": 4,
        "execution_count": 8,
        "parse_error_count": 17,
    },
}
MARKERS = {
    "#new-errors",
    "#document-fragment",
    "#script-off",
    "#script-on",
    "#document",
}


class VerificationError(ValueError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise VerificationError(message)


def load_json(path: Path, label: str) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise VerificationError(f"cannot read {label}: {exc}") from exc


def read_bytes(path: Path, label: str) -> bytes:
    try:
        return path.read_bytes()
    except OSError as exc:
        raise VerificationError(f"cannot read {label}: {exc}") from exc


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def git_blob_sha1(data: bytes) -> str:
    return hashlib.sha1(f"blob {len(data)}\0".encode("ascii") + data).hexdigest()


def require_hex(value: Any, digits: int, label: str) -> str:
    require(isinstance(value, str), f"{label} must be a string")
    require(len(value) == digits, f"{label} must contain exactly {digits} hex digits")
    require(
        all(character in "0123456789abcdef" for character in value),
        f"{label} must be lowercase hexadecimal",
    )
    return value


def require_nonnegative_int(value: Any, label: str) -> int:
    require(
        isinstance(value, int) and not isinstance(value, bool) and value >= 0,
        f"{label} must be a non-negative integer",
    )
    return value


def resolve_vendored_path(root: Path, value: Any, label: str) -> Path:
    require(isinstance(value, str) and value, f"{label} must be a non-empty string")
    relative = Path(value)
    require(not relative.is_absolute(), f"{label} must be repository-relative")
    require(".." not in relative.parts, f"{label} must not escape repository root")
    candidate = (root / relative).resolve()
    try:
        candidate.relative_to(root.resolve())
    except ValueError as exc:
        raise VerificationError(f"{label} escapes repository root") from exc
    return candidate


def require_pinned_bytes(
    path: Path,
    *,
    label: str,
    manifest_size: Any,
    manifest_sha256: Any,
    manifest_git_blob: Any,
    pinned: dict[str, Any],
) -> bytes:
    require(path.is_file(), f"{label} is missing: {path}")
    data = read_bytes(path, label)
    size = require_nonnegative_int(manifest_size, f"{label} size_bytes")
    sha256 = require_hex(manifest_sha256, 64, f"{label} sha256")
    git_blob = require_hex(manifest_git_blob, 40, f"{label} git_blob")

    require(size == pinned["size_bytes"], f"{label} size pin drifted")
    require(sha256 == pinned["sha256"], f"{label} SHA-256 pin drifted")
    require(git_blob == pinned["git_blob"], f"{label} Git blob pin drifted")
    require(len(data) == size, f"{label} size mismatch")
    require(sha256_bytes(data) == sha256, f"{label} SHA-256 mismatch")
    require(git_blob_sha1(data) == git_blob, f"{label} Git blob SHA-1 mismatch")
    return data


def parse_error_lines(lines: list[str], cursor: int, label: str) -> tuple[int, int]:
    count = 0
    while cursor < len(lines):
        line = lines[cursor]
        if line in MARKERS:
            break
        require(line != "", f"{label} contains an empty parse-error line")
        count += 1
        cursor += 1
    return cursor, count


def parse_tree_test(block: str, index: int) -> tuple[int, int, int]:
    label = f"tree test[{index}]"
    lines = block.split("\n")
    require(lines and lines[0] == "#data", f"{label} must begin with #data")

    try:
        errors_index = lines.index("#errors", 1)
    except ValueError as exc:
        raise VerificationError(f"{label} is missing #errors") from exc
    require(errors_index >= 1, f"{label} has invalid #errors placement")

    cursor, error_count = parse_error_lines(lines, errors_index + 1, label)
    if cursor < len(lines) and lines[cursor] == "#new-errors":
        cursor, extra_errors = parse_error_lines(lines, cursor + 1, label)
        error_count += extra_errors

    saw_fragment = False
    if cursor < len(lines) and lines[cursor] == "#document-fragment":
        saw_fragment = True
        cursor += 1
        require(cursor < len(lines), f"{label} document-fragment context is missing")
        context = lines[cursor]
        require(context != "" and not context.startswith("#"),
                f"{label} document-fragment context is invalid")
        cursor += 1

    scripting_mode: str | None = None
    if cursor < len(lines) and lines[cursor] in {"#script-off", "#script-on"}:
        scripting_mode = lines[cursor]
        cursor += 1

    require(cursor < len(lines) and lines[cursor] == "#document",
            f"{label} section order is invalid or #document is missing")
    document_lines = lines[cursor + 1 :]
    require(document_lines, f"{label} document dump is empty")
    require(all(line.startswith("| ") for line in document_lines),
            f"{label} document dump line is malformed")

    executions = 1 if scripting_mode is not None else 2
    return executions, error_count, 1 if saw_fragment else 0


def parse_tree_fixture(data: bytes, label: str) -> tuple[int, int, int, int]:
    try:
        text = data.decode("utf-8")
    except UnicodeError as exc:
        raise VerificationError(f"{label} is not UTF-8: {exc}") from exc
    require(text.endswith("\n"), f"{label} must end with one LF")
    require("\r" not in text, f"{label} must use LF line endings")
    body = text[:-1]
    require(body != "", f"{label} is empty")
    blocks = body.split("\n\n")
    require(all(block != "" for block in blocks), f"{label} has an empty test block")

    executions = 0
    errors = 0
    fragments = 0
    for index, block in enumerate(blocks):
        execution_count, error_count, fragment_count = parse_tree_test(block, index)
        executions += execution_count
        errors += error_count
        fragments += fragment_count
    return len(blocks), executions, errors, fragments


def verify_fixture(path: Path, entry: dict[str, Any], pinned: dict[str, Any]) -> tuple[int, int, int]:
    data = require_pinned_bytes(
        path,
        label="vendored WPT tree fixture",
        manifest_size=entry.get("size_bytes"),
        manifest_sha256=entry.get("sha256"),
        manifest_git_blob=entry.get("git_blob"),
        pinned=pinned,
    )
    tests, executions, errors, fragments = parse_tree_fixture(data, str(path))

    manifest_tests = require_nonnegative_int(entry.get("test_count"), f"{path} test_count")
    manifest_executions = require_nonnegative_int(
        entry.get("execution_count"), f"{path} execution_count"
    )
    manifest_errors = require_nonnegative_int(
        entry.get("parse_error_count"), f"{path} parse_error_count"
    )
    require(manifest_tests == pinned["test_count"], f"{path} test-count pin drifted")
    require(
        manifest_executions == pinned["execution_count"],
        f"{path} execution-count pin drifted",
    )
    require(
        manifest_errors == pinned["parse_error_count"],
        f"{path} parse-error-count pin drifted",
    )
    require(tests == manifest_tests, f"{path} test count mismatch")
    require(executions == manifest_executions, f"{path} execution count mismatch")
    require(errors == manifest_errors, f"{path} parse-error count mismatch")
    return tests, executions, fragments


def verify_manifest(manifest_path: Path, root: Path) -> dict[str, Any]:
    root = root.resolve()
    value = load_json(manifest_path, "Z7 WPT tree corpus manifest")
    require(isinstance(value, dict), "corpus manifest must be an object")
    require(value.get("schema") == SCHEMA, "corpus manifest schema mismatch")
    require(value.get("authority") == AUTHORITY, "corpus manifest authority mismatch")
    require(value.get("conformance_claim") is False,
            "tree corpus preparation must not claim conformance")

    upstream = value.get("upstream")
    require(isinstance(upstream, dict), "corpus manifest upstream must be an object")
    require(upstream.get("repository") == UPSTREAM_REPOSITORY,
            "WPT upstream repository mismatch")
    commit = require_hex(upstream.get("commit"), 40, "WPT upstream commit")
    require(commit == PINNED_UPSTREAM_COMMIT, "WPT upstream commit drifted")
    require(upstream.get("license") == "BSD-3-Clause", "WPT license identifier mismatch")
    require(upstream.get("license_vendored_path") == PINNED_LICENSE["vendored_path"],
            "WPT license path drifted")
    license_path = resolve_vendored_path(
        root, upstream.get("license_vendored_path"), "license_vendored_path"
    )
    require_pinned_bytes(
        license_path,
        label="vendored WPT license",
        manifest_size=upstream.get("license_size_bytes"),
        manifest_sha256=upstream.get("license_sha256"),
        manifest_git_blob=upstream.get("license_git_blob"),
        pinned=PINNED_LICENSE,
    )

    files = value.get("files")
    require(isinstance(files, list) and files, "corpus manifest files must be non-empty array")
    require(len(files) == len(PINNED_FIXTURES), "corpus file set cardinality drifted")

    seen: set[str] = set()
    tests_verified = 0
    executions_verified = 0
    fragments_verified = 0
    for index, entry in enumerate(files):
        label = f"corpus manifest files[{index}]"
        require(isinstance(entry, dict), f"{label} must be an object")
        upstream_path = entry.get("upstream_path")
        require(isinstance(upstream_path, str) and upstream_path in PINNED_FIXTURES,
                f"{label} upstream_path is not admitted by v1 authority")
        require(upstream_path not in seen, f"{label} duplicates upstream path")
        seen.add(upstream_path)

        pinned = PINNED_FIXTURES[upstream_path]
        require(entry.get("vendored_path") == pinned["vendored_path"],
                f"{label} vendored path drifted")
        fixture_path = resolve_vendored_path(root, entry["vendored_path"], f"{label} vendored_path")
        tests, executions, fragments = verify_fixture(fixture_path, entry, pinned)
        tests_verified += tests
        executions_verified += executions
        fragments_verified += fragments

    require(seen == set(PINNED_FIXTURES),
            "corpus manifest does not exactly match admitted upstream file set")

    return {
        "schema": REPORT_SCHEMA,
        "authority": AUTHORITY,
        "upstream_repository": UPSTREAM_REPOSITORY,
        "upstream_commit": commit,
        "files_verified": len(files),
        "tests_verified": tests_verified,
        "executions_verified": executions_verified,
        "fragment_tests_verified": fragments_verified,
        "provenance_gate_passed": True,
        "conformance_claim": False,
    }


def copy_payload(root: Path, temp_root: Path, manifest_value: dict[str, Any]) -> Path:
    manifest = temp_root / DEFAULT_MANIFEST
    manifest.parent.mkdir(parents=True, exist_ok=True)
    manifest.write_text(json.dumps(manifest_value, indent=2) + "\n", encoding="utf-8")

    license_relative = Path(manifest_value["upstream"]["license_vendored_path"])
    (temp_root / license_relative).parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(root / license_relative, temp_root / license_relative)
    for entry in manifest_value["files"]:
        relative = Path(entry["vendored_path"])
        (temp_root / relative).parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(root / relative, temp_root / relative)
    return manifest


def expect_rejection(manifest: Path, root: Path, label: str) -> None:
    try:
        verify_manifest(manifest, root)
    except VerificationError:
        return
    raise VerificationError(f"self-test accepted {label}")


def run_self_test(root: Path, manifest_path: Path) -> dict[str, Any]:
    baseline = verify_manifest(manifest_path, root)
    manifest_value = load_json(manifest_path, "self-test manifest")

    with tempfile.TemporaryDirectory(prefix="zevryon-z7-wpt-tree-") as directory:
        temp_root = Path(directory)
        temp_manifest = copy_payload(root, temp_root, manifest_value)
        verify_manifest(temp_manifest, temp_root)

        fixture_rel = Path(manifest_value["files"][0]["vendored_path"])
        fixture = temp_root / fixture_rel
        fixture.write_bytes(fixture.read_bytes() + b" ")
        expect_rejection(temp_manifest, temp_root, "one-byte WPT fixture tamper")

        shutil.copyfile(root / fixture_rel, fixture)
        drifted = json.loads(temp_manifest.read_text(encoding="utf-8"))
        drifted["upstream"]["commit"] = "0" * 40
        temp_manifest.write_text(json.dumps(drifted, indent=2) + "\n", encoding="utf-8")
        expect_rejection(temp_manifest, temp_root, "WPT commit drift")

        temp_manifest = copy_payload(root, temp_root, manifest_value)
        drifted = json.loads(temp_manifest.read_text(encoding="utf-8"))
        drifted["files"][0]["git_blob"] = "0" * 40
        temp_manifest.write_text(json.dumps(drifted, indent=2) + "\n", encoding="utf-8")
        expect_rejection(temp_manifest, temp_root, "WPT Git blob drift")

        malformed = b"#data\n<p>x\n#document\n| <html>\n#errors\nbad\n"
        try:
            parse_tree_fixture(malformed, "malformed self-test fixture")
        except VerificationError:
            pass
        else:
            raise VerificationError("self-test accepted malformed WPT section order")

    return {
        **baseline,
        "self_test_passed": True,
        "tamper_rejection_checked": True,
        "commit_drift_rejection_checked": True,
        "git_blob_drift_rejection_checked": True,
        "format_rejection_checked": True,
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Verify frozen WPT tree-construction corpus provenance for Z7"
    )
    parser.add_argument("--root", type=Path, default=None)
    parser.add_argument("--manifest", type=Path, default=None)
    parser.add_argument("--self-test", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    root = (
        args.root if args.root is not None else Path(__file__).resolve().parents[1]
    ).resolve()
    manifest = (
        args.manifest if args.manifest is not None else root / DEFAULT_MANIFEST
    ).resolve()
    try:
        report = run_self_test(root, manifest) if args.self_test else verify_manifest(manifest, root)
    except VerificationError as exc:
        print(f"Z7 WPT tree corpus verification failed: {exc}", file=sys.stderr)
        return 1
    print(json.dumps(report, sort_keys=True, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
