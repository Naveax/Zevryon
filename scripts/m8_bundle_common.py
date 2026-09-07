#!/usr/bin/env python3
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re
import subprocess
from typing import Any, Iterable

PLAN_SCHEMA = "zevryon.m8.bundle-plan.v1"
PLAN_AUTHORITY = "m8-pre-frozen-single-bundle-plan-v1"
RECEIPT_SCHEMA = "zevryon.m8.bundle-artifact-receipt.v1"
FINAL_SCHEMA = "zevryon.m8.final-certification.v1"
FINAL_AUTHORITY = "m8-no-compensation-raw-evidence-binder-v1"

HEX40 = re.compile(r"^[0-9a-f]{40}$")
HEX64 = re.compile(r"^[0-9a-f]{64}$")
BUNDLE_ID = re.compile(r"^[0-9a-f]{32}$")

ARTIFACT_PATHS = {
    "profile": "profile-observations.json",
    "storage_crash": "storage-process-crash.json",
    "mixed_mutation": "mixed-mutation.json",
    "soak": "continuous-soak.jsonl",
    "property_fuzz": "property-fuzz.json",
}
RECEIPT_PATHS = {
    "profile": "receipts/profile-observations.json",
    "storage_crash": "receipts/storage-process-crash.json",
    "mixed_mutation": "receipts/mixed-mutation.json",
    "soak": "receipts/continuous-soak.json",
    "property_fuzz": "receipts/property-fuzz.json",
}
FINAL_OUTPUT_PATH = "final-certification.json"
PLAN_FILENAME = "bundle-plan.json"

AUTHORITY_SOURCE_FILES = (
    "zevryon_platform/performance_contract.py",
    "scripts/m8_profile_observation_gate.py",
    "scripts/m8_storage_process_crash_tests.py",
    "tests/m8_storage_crash_probe.cpp",
    "tests/m8_mixed_mutation_probe.cpp",
    "tests/m8_continuous_soak_probe.cpp",
    "tests/m8_property_fuzz_probe.cpp",
    "scripts/m8_bundle_common.py",
    "scripts/m8_freeze_bundle.py",
    "scripts/m8_bundle_run.py",
    "scripts/m8_bundle_import_profile.py",
    "scripts/m8_final_evidence_binder.py",
)

PLAN_FIELDS = {
    "schema",
    "authority",
    "bundle_id",
    "candidate_commit",
    "candidate_tree",
    "created_utc",
    "artifacts",
    "receipts",
    "final_output",
    "authority_source_sha256",
}


class EvidenceInvalid(RuntimeError):
    """The evidence structure or immutable binding is invalid."""


class GateFailed(RuntimeError):
    """The evidence is valid but one or more required certification gates failed."""


def require(condition: bool, message: str) -> None:
    if not condition:
        raise EvidenceInvalid(message)


def canonical_json(value: Any) -> str:
    return json.dumps(value, indent=2, sort_keys=True, ensure_ascii=False) + "\n"


def load_json_bytes(raw: bytes, context: str) -> Any:
    try:
        text = raw.decode("utf-8", errors="strict")
    except UnicodeDecodeError as exc:
        raise EvidenceInvalid(f"{context} is not valid UTF-8") from exc
    try:
        return json.loads(
            text,
            parse_constant=lambda value: (_ for _ in ()).throw(
                EvidenceInvalid(f"{context} contains forbidden non-finite JSON constant: {value}")
            ),
        )
    except json.JSONDecodeError as exc:
        raise EvidenceInvalid(f"{context} is invalid JSON: {exc}") from exc


def load_json_path(path: Path, context: str) -> tuple[Any, bytes]:
    try:
        raw = path.read_bytes()
    except OSError as exc:
        raise EvidenceInvalid(f"cannot read {context}: {path}: {exc}") from exc
    return load_json_bytes(raw, context), raw


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as handle:
            while True:
                chunk = handle.read(1024 * 1024)
                if not chunk:
                    break
                digest.update(chunk)
    except OSError as exc:
        raise EvidenceInvalid(f"cannot hash file {path}: {exc}") from exc
    return digest.hexdigest()


def exact_keys(value: Any, expected: Iterable[str], context: str) -> dict[str, Any]:
    require(isinstance(value, dict), f"{context} must be an object")
    expected_set = set(expected)
    actual = set(value)
    missing = sorted(expected_set - actual)
    extra = sorted(actual - expected_set)
    require(not missing, f"{context} missing fields: {', '.join(missing)}")
    require(not extra, f"{context} contains unexpected fields: {', '.join(extra)}")
    return value


def require_int(value: Any, context: str, minimum: int = 0) -> int:
    require(type(value) is int, f"{context} must be an integer")
    require(value >= minimum, f"{context} must be >= {minimum}")
    return value


def require_bool(value: Any, context: str) -> bool:
    require(type(value) is bool, f"{context} must be a boolean")
    return value


def require_str(value: Any, context: str) -> str:
    require(isinstance(value, str), f"{context} must be a string")
    return value


def require_hex40(value: Any, context: str) -> str:
    text = require_str(value, context)
    require(HEX40.fullmatch(text) is not None, f"{context} must be lowercase 40-hex")
    return text


def require_hex64(value: Any, context: str) -> str:
    text = require_str(value, context)
    require(HEX64.fullmatch(text) is not None, f"{context} must be lowercase 64-hex")
    return text


def source_root() -> Path:
    return Path(__file__).resolve().parents[1]


def run_git(*args: str) -> str:
    try:
        result = subprocess.run(
            ["git", "-C", str(source_root()), *args],
            text=True,
            encoding="utf-8",
            errors="strict",
            capture_output=True,
            check=False,
            timeout=20.0,
        )
    except (OSError, subprocess.SubprocessError) as exc:
        raise EvidenceInvalid(f"git command failed to execute: {' '.join(args)}: {exc}") from exc
    require(result.returncode == 0, f"git {' '.join(args)} failed: {result.stderr.strip()}")
    return result.stdout.strip()


def clean_git_identity() -> tuple[str, str]:
    status = run_git("status", "--porcelain=v1", "--untracked-files=all")
    require(status == "", "repository must be clean before bundle evidence is frozen or admitted")
    commit = require_hex40(run_git("rev-parse", "HEAD"), "current Git commit")
    tree = require_hex40(run_git("rev-parse", "HEAD^{tree}"), "current Git tree")
    return commit, tree


def authority_source_hashes() -> dict[str, str]:
    root = source_root()
    result: dict[str, str] = {}
    for relative in AUTHORITY_SOURCE_FILES:
        path = (root / relative).resolve()
        require(path.is_file(), f"authority source is missing: {relative}")
        require(path.is_relative_to(root.resolve()), f"authority source escaped repository root: {relative}")
        result[relative] = sha256_file(path)
    return result


def resolve_contained(root: Path, relative: str, context: str) -> Path:
    require(isinstance(relative, str) and relative != "", f"{context} path must be non-empty")
    rel = Path(relative)
    require(not rel.is_absolute(), f"{context} path must be relative")
    require(".." not in rel.parts, f"{context} path may not contain traversal")
    root_resolved = root.resolve()
    resolved = (root_resolved / rel).resolve(strict=False)
    require(resolved.is_relative_to(root_resolved), f"{context} resolved outside artifact root")
    return resolved


def validate_bundle_plan(document: Any, artifact_root: Path, verify_current: bool = True) -> dict[str, Any]:
    plan = exact_keys(document, PLAN_FIELDS, "bundle plan")
    require(plan["schema"] == PLAN_SCHEMA, "bundle plan schema mismatch")
    require(plan["authority"] == PLAN_AUTHORITY, "bundle plan authority mismatch")
    bundle_id = require_str(plan["bundle_id"], "bundle_id")
    require(BUNDLE_ID.fullmatch(bundle_id) is not None, "bundle_id must be lowercase 32-hex")
    candidate_commit = require_hex40(plan["candidate_commit"], "candidate_commit")
    candidate_tree = require_hex40(plan["candidate_tree"], "candidate_tree")
    require(isinstance(plan["created_utc"], str) and plan["created_utc"].endswith("Z"), "created_utc must be UTC text ending in Z")

    require(plan["artifacts"] == ARTIFACT_PATHS, "bundle plan artifact path contract drifted")
    require(plan["receipts"] == RECEIPT_PATHS, "bundle plan receipt path contract drifted")
    require(plan["final_output"] == FINAL_OUTPUT_PATH, "bundle plan final output path contract drifted")

    hashes = plan["authority_source_sha256"]
    require(isinstance(hashes, dict), "authority_source_sha256 must be an object")
    require(set(hashes) == set(AUTHORITY_SOURCE_FILES), "authority source file set drifted")
    for key, value in hashes.items():
        require_hex64(value, f"authority source hash {key}")

    root = artifact_root.resolve()
    repository = source_root().resolve()
    require(root != repository and not root.is_relative_to(repository), "artifact root must be outside the repository")

    resolved_paths: list[Path] = []
    for key, relative in ARTIFACT_PATHS.items():
        resolved_paths.append(resolve_contained(root, relative, f"artifact {key}"))
    for key, relative in RECEIPT_PATHS.items():
        resolved_paths.append(resolve_contained(root, relative, f"receipt {key}"))
    resolved_paths.append(resolve_contained(root, FINAL_OUTPUT_PATH, "final output"))
    resolved_paths.append(resolve_contained(root, PLAN_FILENAME, "bundle plan"))
    require(len(set(resolved_paths)) == len(resolved_paths), "bundle paths collide after canonical resolution")

    if verify_current:
        current_commit, current_tree = clean_git_identity()
        require(current_commit == candidate_commit, "current Git commit does not match frozen bundle candidate")
        require(current_tree == candidate_tree, "current Git tree does not match frozen bundle candidate")
        require(authority_source_hashes() == hashes, "authority source hashes changed after bundle freeze")
    return plan


def load_bundle_plan(artifact_root: Path, verify_current: bool = True) -> tuple[dict[str, Any], bytes]:
    root = artifact_root.resolve()
    plan_path = resolve_contained(root, PLAN_FILENAME, "bundle plan")
    document, raw = load_json_path(plan_path, "bundle plan")
    return validate_bundle_plan(document, root, verify_current=verify_current), raw


def artifact_path(artifact_root: Path, plan: dict[str, Any], key: str) -> Path:
    require(key in ARTIFACT_PATHS, f"unknown artifact key: {key}")
    return resolve_contained(artifact_root, plan["artifacts"][key], f"artifact {key}")


def receipt_path(artifact_root: Path, plan: dict[str, Any], key: str) -> Path:
    require(key in RECEIPT_PATHS, f"unknown receipt key: {key}")
    return resolve_contained(artifact_root, plan["receipts"][key], f"receipt {key}")


def exclusive_write(path: Path, text: str) -> None:
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("x", encoding="utf-8", newline="\n") as handle:
            handle.write(text)
    except FileExistsError as exc:
        raise EvidenceInvalid(f"refusing to overwrite immutable bundle file: {path}") from exc
    except OSError as exc:
        raise EvidenceInvalid(f"cannot write immutable bundle file {path}: {exc}") from exc
