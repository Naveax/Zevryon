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

SCHEMA = "zevryon.z7.html5lib-tokenizer-corpus.v1"
AUTHORITY = "z7-html5lib-tokenizer-corpus-provenance-v1"
REPORT_SCHEMA = "zevryon.z7.html5lib-tokenizer-corpus-verification.v1"
UPSTREAM_REPOSITORY = "html5lib/html5lib-tests"
PINNED_UPSTREAM_COMMIT = "224991ec10db04f056a89eed8b0bd8695fd2950e"
DEFAULT_MANIFEST = Path("config/z7_html5lib_tokenizer_corpus.json")

PINNED_LICENSE = {
    "vendored_path": "third_party/html5lib-tests/LICENSE",
    "git_blob": "8812371b41cfc6d2de3eac8c0b2a7a90f9b03428",
    "size_bytes": 1103,
    "sha256": "ff512aac9ef231d504be5afaf4429005024e4b2aaf257be39524f37b8402aaf2",
}
PINNED_FIXTURES = {
    "tokenizer/contentModelFlags.test": {
        "vendored_path": "tests/fixtures/html5lib-tokenizer/contentModelFlags.test",
        "git_blob": "9cf7c8bd9e70dfbd0037726d6a840e67d3aa5e12",
        "size_bytes": 3055,
        "sha256": "77784a505a528950761cfb3c76617afade28b27c3be2a8c37dce3c3d8988391d",
        "test_count": 14,
        "execution_count": 24,
    },
}

ALLOWED_INITIAL_STATES = {
    "Data state",
    "PLAINTEXT state",
    "RCDATA state",
    "RAWTEXT state",
    "Script data state",
    "CDATA section state",
}
ALLOWED_TOKEN_TYPES = {
    "DOCTYPE",
    "StartTag",
    "EndTag",
    "Comment",
    "Character",
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
    header = f"blob {len(data)}\0".encode("ascii")
    return hashlib.sha1(header + data).hexdigest()


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
    require(".." not in relative.parts, f"{label} must not escape the repository root")
    candidate = (root / relative).resolve()
    try:
        candidate.relative_to(root.resolve())
    except ValueError as exc:
        raise VerificationError(f"{label} escapes the repository root") from exc
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

    manifest_size_int = require_nonnegative_int(manifest_size, f"{label} size_bytes")
    manifest_sha = require_hex(manifest_sha256, 64, f"{label} sha256")
    manifest_blob = require_hex(manifest_git_blob, 40, f"{label} git_blob")

    require(manifest_size_int == pinned["size_bytes"], f"{label} size pin drifted")
    require(manifest_sha == pinned["sha256"], f"{label} SHA-256 pin drifted")
    require(manifest_blob == pinned["git_blob"], f"{label} Git blob pin drifted")

    require(len(data) == manifest_size_int, f"{label} size mismatch")
    require(sha256_bytes(data) == manifest_sha, f"{label} SHA-256 mismatch")
    require(git_blob_sha1(data) == manifest_blob, f"{label} Git blob SHA-1 mismatch")
    return data


def verify_output_token(token: Any, case_label: str, token_index: int) -> None:
    label = f"{case_label} output[{token_index}]"
    require(isinstance(token, list) and token, f"{label} must be a non-empty array")
    kind = token[0]
    require(kind in ALLOWED_TOKEN_TYPES, f"{label} has unsupported token type: {kind!r}")

    if kind in {"EndTag", "Comment", "Character"}:
        require(
            len(token) == 2 and isinstance(token[1], str),
            f"{label} has invalid {kind} shape",
        )
        return

    if kind == "StartTag":
        require(len(token) in {3, 4}, f"{label} has invalid StartTag length")
        require(
            isinstance(token[1], str) and isinstance(token[2], dict),
            f"{label} has invalid StartTag name/attributes",
        )
        require(
            all(
                isinstance(key, str) and isinstance(value, str)
                for key, value in token[2].items()
            ),
            f"{label} attributes must be string-to-string",
        )
        if len(token) == 4:
            require(token[3] is True, f"{label} self-closing flag must be true when present")
        return

    require(len(token) == 5, f"{label} has invalid DOCTYPE length")
    require(isinstance(token[1], str), f"{label} DOCTYPE name must be a string")
    require(
        token[2] is None or isinstance(token[2], str),
        f"{label} DOCTYPE public id must be string/null",
    )
    require(
        token[3] is None or isinstance(token[3], str),
        f"{label} DOCTYPE system id must be string/null",
    )
    require(isinstance(token[4], bool), f"{label} DOCTYPE correctness must be boolean")


def verify_test_case(test: Any, file_label: str, index: int) -> int:
    label = f"{file_label} test[{index}]"
    require(isinstance(test, dict), f"{label} must be an object")
    for field in ("description", "input", "output"):
        require(field in test, f"{label} is missing required field {field!r}")

    require(
        isinstance(test["description"], str) and test["description"],
        f"{label} description must be non-empty string",
    )
    require(isinstance(test["input"], str), f"{label} input must be a string")
    require(isinstance(test["output"], list), f"{label} output must be an array")
    for token_index, token in enumerate(test["output"]):
        verify_output_token(token, label, token_index)

    states = test.get("initialStates", ["Data state"])
    require(
        isinstance(states, list) and states,
        f"{label} initialStates must be non-empty array",
    )
    require(
        all(isinstance(state, str) and state in ALLOWED_INITIAL_STATES for state in states),
        f"{label} contains unknown tokenizer initial state",
    )
    require(len(states) == len(set(states)), f"{label} initialStates contains duplicates")

    if "lastStartTag" in test:
        last_start_tag = test["lastStartTag"]
        require(isinstance(last_start_tag, str), f"{label} lastStartTag must be string")
        require(last_start_tag == last_start_tag.lower(), f"{label} lastStartTag must be lowercase")

    if "doubleEscaped" in test:
        require(
            isinstance(test["doubleEscaped"], bool),
            f"{label} doubleEscaped must be boolean",
        )

    errors = test.get("errors", [])
    require(isinstance(errors, list), f"{label} errors must be an array")
    for error_index, error in enumerate(errors):
        error_label = f"{label} errors[{error_index}]"
        require(isinstance(error, dict), f"{error_label} must be an object")
        require(
            isinstance(error.get("code"), str) and error["code"],
            f"{error_label} code must be non-empty string",
        )
        for coordinate in ("line", "col"):
            value = error.get(coordinate)
            require(
                isinstance(value, int) and not isinstance(value, bool) and value >= 1,
                f"{error_label} {coordinate} must be positive integer",
            )

    return len(states)


def verify_fixture(path: Path, entry: dict[str, Any], pinned: dict[str, Any]) -> tuple[int, int]:
    data = require_pinned_bytes(
        path,
        label="vendored tokenizer fixture",
        manifest_size=entry.get("size_bytes"),
        manifest_sha256=entry.get("sha256"),
        manifest_git_blob=entry.get("git_blob"),
        pinned=pinned,
    )

    try:
        value = json.loads(data.decode("utf-8"))
    except (UnicodeError, json.JSONDecodeError) as exc:
        raise VerificationError(f"cannot parse tokenizer fixture {path}: {exc}") from exc

    require(isinstance(value, dict), f"{path} top-level value must be object")
    tests = value.get("tests")
    require(isinstance(tests, list), f"{path} must contain top-level tests array")

    manifest_test_count = require_nonnegative_int(entry.get("test_count"), f"{path} test_count")
    manifest_execution_count = require_nonnegative_int(
        entry.get("execution_count"), f"{path} execution_count"
    )
    require(manifest_test_count == pinned["test_count"], f"{path} test-count pin drifted")
    require(
        manifest_execution_count == pinned["execution_count"],
        f"{path} execution-count pin drifted",
    )
    require(len(tests) == manifest_test_count, f"{path} test count mismatch")

    execution_count = 0
    for index, test in enumerate(tests):
        execution_count += verify_test_case(test, str(path), index)
    require(
        execution_count == manifest_execution_count,
        f"{path} initial-state execution count mismatch",
    )
    return len(tests), execution_count


def verify_manifest(manifest_path: Path, root: Path) -> dict[str, Any]:
    root = root.resolve()
    value = load_json(manifest_path, "Z7 html5lib tokenizer corpus manifest")
    require(isinstance(value, dict), "corpus manifest must be an object")
    require(value.get("schema") == SCHEMA, "corpus manifest schema mismatch")
    require(value.get("authority") == AUTHORITY, "corpus manifest authority mismatch")
    require(
        value.get("conformance_claim") is False,
        "corpus preparation manifest must not claim conformance",
    )

    upstream = value.get("upstream")
    require(isinstance(upstream, dict), "corpus manifest upstream must be an object")
    require(
        upstream.get("repository") == UPSTREAM_REPOSITORY,
        "html5lib tokenizer upstream repository mismatch",
    )
    commit = require_hex(upstream.get("commit"), 40, "html5lib tokenizer upstream commit")
    require(
        commit == PINNED_UPSTREAM_COMMIT,
        "html5lib tokenizer upstream commit drifted from admitted pin",
    )
    require(upstream.get("license") == "MIT", "html5lib tokenizer license identifier mismatch")
    require(
        upstream.get("license_vendored_path") == PINNED_LICENSE["vendored_path"],
        "html5lib tokenizer license path drifted",
    )
    license_path = resolve_vendored_path(
        root, upstream.get("license_vendored_path"), "license_vendored_path"
    )
    require_pinned_bytes(
        license_path,
        label="vendored html5lib tokenizer license",
        manifest_size=upstream.get("license_size_bytes"),
        manifest_sha256=upstream.get("license_sha256"),
        manifest_git_blob=upstream.get("license_git_blob"),
        pinned=PINNED_LICENSE,
    )

    files = value.get("files")
    require(isinstance(files, list) and files, "corpus manifest files must be non-empty array")
    require(len(files) == len(PINNED_FIXTURES), "corpus manifest file set cardinality drifted")

    seen_upstream_paths: set[str] = set()
    seen_vendored_paths: set[str] = set()
    tests_verified = 0
    executions_verified = 0

    for index, entry in enumerate(files):
        label = f"corpus manifest files[{index}]"
        require(isinstance(entry, dict), f"{label} must be an object")
        upstream_path = entry.get("upstream_path")
        require(
            isinstance(upstream_path, str) and upstream_path in PINNED_FIXTURES,
            f"{label} upstream_path is not admitted by v1 authority",
        )
        require(upstream_path not in seen_upstream_paths, f"{label} duplicates upstream path")
        seen_upstream_paths.add(upstream_path)

        pinned = PINNED_FIXTURES[upstream_path]
        vendored_path = entry.get("vendored_path")
        require(
            vendored_path == pinned["vendored_path"],
            f"{label} vendored_path drifted from admitted mapping",
        )
        require(vendored_path not in seen_vendored_paths, f"{label} duplicates vendored path")
        seen_vendored_paths.add(vendored_path)

        fixture_path = resolve_vendored_path(root, vendored_path, f"{label} vendored_path")
        tests, executions = verify_fixture(fixture_path, entry, pinned)
        tests_verified += tests
        executions_verified += executions

    require(
        seen_upstream_paths == set(PINNED_FIXTURES),
        "corpus manifest does not exactly match admitted upstream file set",
    )

    return {
        "schema": REPORT_SCHEMA,
        "authority": AUTHORITY,
        "upstream_repository": UPSTREAM_REPOSITORY,
        "upstream_commit": commit,
        "files_verified": len(files),
        "tests_verified": tests_verified,
        "executions_verified": executions_verified,
        "provenance_gate_passed": True,
        "conformance_claim": False,
    }


def copy_manifest_payload(root: Path, temp_root: Path, manifest_value: dict[str, Any]) -> Path:
    temp_manifest = temp_root / DEFAULT_MANIFEST
    temp_manifest.parent.mkdir(parents=True, exist_ok=True)
    temp_manifest.write_text(json.dumps(manifest_value, indent=2) + "\n", encoding="utf-8")

    upstream = manifest_value["upstream"]
    license_relative = Path(upstream["license_vendored_path"])
    (temp_root / license_relative).parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(root / license_relative, temp_root / license_relative)

    for entry in manifest_value["files"]:
        relative = Path(entry["vendored_path"])
        (temp_root / relative).parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(root / relative, temp_root / relative)

    return temp_manifest


def expect_rejection(manifest_path: Path, root: Path, label: str) -> None:
    try:
        verify_manifest(manifest_path, root)
    except VerificationError:
        return
    raise VerificationError(f"self-test accepted {label}")


def run_self_test(root: Path, manifest_path: Path) -> dict[str, Any]:
    baseline = verify_manifest(manifest_path, root)
    manifest_value = load_json(manifest_path, "self-test manifest")

    with tempfile.TemporaryDirectory(prefix="zevryon-z7-html5lib-") as directory:
        temp_root = Path(directory)
        temp_manifest = copy_manifest_payload(root, temp_root, manifest_value)
        verify_manifest(temp_manifest, temp_root)

        first_entry = manifest_value["files"][0]
        first_fixture = temp_root / Path(first_entry["vendored_path"])
        first_fixture.write_bytes(first_fixture.read_bytes() + b" ")
        expect_rejection(temp_manifest, temp_root, "one-byte tokenizer fixture tamper")

        shutil.copyfile(root / Path(first_entry["vendored_path"]), first_fixture)
        drifted = json.loads(temp_manifest.read_text(encoding="utf-8"))
        drifted["upstream"]["commit"] = "0" * 40
        temp_manifest.write_text(json.dumps(drifted, indent=2) + "\n", encoding="utf-8")
        expect_rejection(temp_manifest, temp_root, "upstream commit drift")

        temp_manifest = copy_manifest_payload(root, temp_root, manifest_value)
        drifted = json.loads(temp_manifest.read_text(encoding="utf-8"))
        drifted["files"][0]["git_blob"] = "0" * 40
        temp_manifest.write_text(json.dumps(drifted, indent=2) + "\n", encoding="utf-8")
        expect_rejection(temp_manifest, temp_root, "upstream Git blob drift")

    return {
        **baseline,
        "self_test_passed": True,
        "tamper_rejection_checked": True,
        "commit_drift_rejection_checked": True,
        "git_blob_drift_rejection_checked": True,
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Verify frozen html5lib tokenizer corpus provenance for Z7"
    )
    parser.add_argument(
        "--root",
        type=Path,
        default=None,
        help="repository root; defaults to parent of scripts/",
    )
    parser.add_argument(
        "--manifest",
        type=Path,
        default=None,
        help="manifest path; defaults to config/z7_html5lib_tokenizer_corpus.json",
    )
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="also prove fixture-tamper, commit-drift and Git-blob-drift rejection",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    root = (
        args.root if args.root is not None else Path(__file__).resolve().parents[1]
    ).resolve()
    manifest_path = (
        args.manifest if args.manifest is not None else root / DEFAULT_MANIFEST
    ).resolve()
    try:
        report = (
            run_self_test(root, manifest_path)
            if args.self_test
            else verify_manifest(manifest_path, root)
        )
    except VerificationError as exc:
        print(f"Z7 html5lib tokenizer corpus verification failed: {exc}", file=sys.stderr)
        return 1

    print(json.dumps(report, sort_keys=True, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
