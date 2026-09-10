#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any

SCHEMA = "zevryon.z7.html-named-reference-table.v1"
AUTHORITY = "z7-html-named-reference-table-provenance-v1"
UPSTREAM_REPOSITORY = "whatwg/html-build"
UPSTREAM_COMMIT = "283a3531a61106d07d9a7d9fb3e6f3b9bfd33d70"
UPSTREAM_PATH = "entities/out/entities.json"
UPSTREAM_GIT_BLOB = "557170b41f47a13a46ec695561eb5fe76da73bdb"
SOURCE_SIZE = 145897
SOURCE_SHA256 = "d741d877ac77c4194c4ad526b5b4a19aef8dfe411ab840a466891cdbb9f362e6"
ENTRY_COUNT = 2231
LEGACY_NAME_COUNT = 106
TWO_SCALAR_COUNT = 93
MAXIMUM_NAME_BYTES = 32
LICENSE_GIT_BLOB = "f2dcda46deccefd245749202a88a7837e35c6daa"
SOURCE_REL = Path("third_party/whatwg-html-build/entities.json")
LICENSE_REL = Path("third_party/whatwg-html-build/LICENSE")
GENERATOR_REL = Path("scripts/generate_html_named_character_references_v1.py")
MANIFEST_REL = Path("config/z7_html_named_reference_table.json")

GENERATED_SHA256 = {
    "src/html_named_character_references_v1.generated.hpp": "4c20aee30406c1f07c52ce8fd0f1be42b7187d451fd6d90ca1e9d3a568e6e047",
    "src/html_named_character_references_v1.generated_part0.inc": "bfc1611dfba2764d9ae7a8cc2a1d314a7da58fe4157d542a8a5295bac7daa562",
    "src/html_named_character_references_v1.generated_part1.inc": "b452492d9469d5791b88ae5f3ed5f1ddeb589ae1bb4ac67f51e91a98a5448774",
    "src/html_named_character_references_v1.generated_part2.inc": "2d09a9f84c0161d04bcfa774a9d1bb72e103944ca58db09ff931c6542a009ac2",
    "src/html_named_character_references_v1.generated_part3.inc": "82ada17129d658eace6d8ef1dd87f8cb7aac68196612b928753a1c7d075bdbfb",
    "src/html_named_character_references_v1.generated_part4.inc": "c95530729bb1c5bbf310c41f292d3622445074594c374de616a33f00ef31fe49",
    "src/html_named_character_references_v1.generated_part5.inc": "aac58edfecb5a31bc7d091d4fda40249011dfe8afcc5ba2778e9a92cda8fc9ea",
    "src/html_named_character_references_v1.generated_part6.inc": "63c128f65b783f79533642a822a91fa0cbd1193dfc5fe4a9243eb780ff4eefea",
    "src/html_named_character_references_v1.generated_part7.inc": "0f4be6810c06ce0b7ac7b29ab97e0d33fd64302b979d0de8e1c4767658894f0e",
}


class VerificationError(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise VerificationError(message)


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def git_blob_sha1(data: bytes) -> str:
    header = b"blob " + str(len(data)).encode("ascii") + b"\0"
    return hashlib.sha1(header + data).hexdigest()


def load_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise VerificationError(f"cannot load JSON {path}: {exc}") from exc


def expected_manifest() -> dict[str, Any]:
    return {
        "schema": SCHEMA,
        "authority": AUTHORITY,
        "conformance_claim": False,
        "upstream": {
            "repository": UPSTREAM_REPOSITORY,
            "commit": UPSTREAM_COMMIT,
            "path": UPSTREAM_PATH,
            "git_blob": UPSTREAM_GIT_BLOB,
            "size_bytes": SOURCE_SIZE,
            "sha256": SOURCE_SHA256,
            "entry_count": ENTRY_COUNT,
            "legacy_name_count": LEGACY_NAME_COUNT,
            "two_scalar_count": TWO_SCALAR_COUNT,
            "maximum_name_bytes": MAXIMUM_NAME_BYTES,
            "vendored_path": SOURCE_REL.as_posix(),
        },
        "license": {
            "upstream_path": "LICENSE",
            "git_blob": LICENSE_GIT_BLOB,
            "vendored_path": LICENSE_REL.as_posix(),
        },
        "generator": GENERATOR_REL.as_posix(),
        "generated_sha256": GENERATED_SHA256,
    }


def validate_manifest(root: Path) -> None:
    manifest = load_json(root / MANIFEST_REL)
    require(manifest == expected_manifest(), "named-reference provenance manifest drifted from v1 authority")


def validate_source(root: Path) -> dict[str, int]:
    source_path = root / SOURCE_REL
    raw = source_path.read_bytes()
    require(len(raw) == SOURCE_SIZE, f"entities.json size mismatch: {len(raw)} != {SOURCE_SIZE}")
    require(sha256_bytes(raw) == SOURCE_SHA256, "entities.json SHA-256 mismatch")
    require(git_blob_sha1(raw) == UPSTREAM_GIT_BLOB, "entities.json Git blob mismatch")

    payload = load_json(source_path)
    require(isinstance(payload, dict), "entities.json root must be an object")
    require(len(payload) == ENTRY_COUNT, "entities.json entry count mismatch")

    legacy = 0
    two_scalar = 0
    maximum = 0
    for name, record in payload.items():
        require(isinstance(name, str) and name.startswith("&") and len(name) > 1,
                "entities.json contains invalid entity key")
        entity_name = name[1:]
        require(all(ord(ch) < 0x80 for ch in entity_name),
                f"entity name is not ASCII: {name!r}")
        maximum = max(maximum, len(entity_name.encode("ascii")))
        if not entity_name.endswith(";"):
            legacy += 1
        require(isinstance(record, dict), f"entity record is not an object: {name}")
        codepoints = record.get("codepoints")
        require(isinstance(codepoints, list) and len(codepoints) in (1, 2),
                f"entity codepoint count invalid: {name}")
        require(all(isinstance(value, int) and not isinstance(value, bool) for value in codepoints),
                f"entity codepoints are not integers: {name}")
        for value in codepoints:
            require(0 <= value <= 0x10FFFF and not 0xD800 <= value <= 0xDFFF,
                    f"entity contains invalid Unicode scalar: {name}")
        if len(codepoints) == 2:
            two_scalar += 1

    require(legacy == LEGACY_NAME_COUNT, f"legacy-name count mismatch: {legacy}")
    require(two_scalar == TWO_SCALAR_COUNT, f"two-scalar count mismatch: {two_scalar}")
    require(maximum == MAXIMUM_NAME_BYTES, f"maximum name length mismatch: {maximum}")
    return {"entries": len(payload), "legacy": legacy, "two_scalar": two_scalar, "maximum": maximum}


def validate_license(root: Path) -> None:
    raw = (root / LICENSE_REL).read_bytes()
    require(git_blob_sha1(raw) == LICENSE_GIT_BLOB, "vendored WHATWG license Git blob mismatch")


def validate_checked_in_generated(root: Path) -> None:
    expected_paths = {Path(path) for path in GENERATED_SHA256}
    actual_parts = set((root / "src").glob("html_named_character_references_v1.generated_part*.inc"))
    actual_paths = {path.relative_to(root) for path in actual_parts}
    require(
        actual_paths == {path for path in expected_paths if path.suffix == ".inc"},
        "generated named-reference part set must remain exactly 8 files",
    )
    for relative, expected_digest in GENERATED_SHA256.items():
        path = root / relative
        require(path.is_file(), f"missing generated named-reference file: {relative}")
        require(sha256_bytes(path.read_bytes()) == expected_digest,
                f"generated named-reference digest mismatch: {relative}")


def regenerate_and_compare(root: Path) -> None:
    generator = root / GENERATOR_REL
    require(generator.is_file(), "named-reference generator is missing")
    with tempfile.TemporaryDirectory(prefix="zevryon-z7-named-ref-") as tmp:
        output_dir = Path(tmp) / "generated"
        completed = subprocess.run(
            [
                sys.executable,
                str(generator),
                "--entities-json",
                str(root / SOURCE_REL),
                "--output-dir",
                str(output_dir),
            ],
            cwd=root,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )
        require(completed.returncode == 0,
                f"named-reference generator failed: {completed.stderr.strip()}")
        for relative in GENERATED_SHA256:
            name = Path(relative).name
            regenerated = output_dir / name
            checked_in = root / relative
            require(regenerated.is_file(), f"generator omitted {name}")
            require(regenerated.read_bytes() == checked_in.read_bytes(),
                    f"generator output differs from checked-in file: {name}")


def verify_root(root: Path) -> dict[str, Any]:
    validate_manifest(root)
    source_stats = validate_source(root)
    validate_license(root)
    validate_checked_in_generated(root)
    regenerate_and_compare(root)
    return {
        "schema": "zevryon.z7.html-named-reference-table-verification.v1",
        "authority": AUTHORITY,
        "provenance_gate_passed": True,
        "conformance_claim": False,
        "source": source_stats,
        "generated_files_verified": len(GENERATED_SHA256),
    }


def copy_authority_tree(source_root: Path, destination_root: Path) -> None:
    relatives = [MANIFEST_REL, SOURCE_REL, LICENSE_REL, GENERATOR_REL]
    relatives.extend(Path(path) for path in GENERATED_SHA256)
    for relative in relatives:
        source = source_root / relative
        destination = destination_root / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)


def expect_rejection(root: Path, label: str) -> None:
    try:
        verify_root(root)
    except VerificationError:
        return
    raise VerificationError(f"self-test mutation was not rejected: {label}")


def self_test(root: Path) -> None:
    verify_root(root)
    with tempfile.TemporaryDirectory(prefix="zevryon-z7-named-ref-selftest-") as tmp:
        temp_root = Path(tmp) / "repo"
        copy_authority_tree(root, temp_root)

        source_path = temp_root / SOURCE_REL
        source_original = source_path.read_bytes()
        source_path.write_bytes(source_original + b"\n")
        expect_rejection(temp_root, "source-byte mutation")
        source_path.write_bytes(source_original)

        part_path = temp_root / "src/html_named_character_references_v1.generated_part7.inc"
        part_original = part_path.read_bytes()
        part_path.write_bytes(part_original + b"// drift\n")
        expect_rejection(temp_root, "generated-output mutation")
        part_path.write_bytes(part_original)

        manifest_path = temp_root / MANIFEST_REL
        manifest_original = manifest_path.read_text(encoding="utf-8")
        manifest_path.write_text(manifest_original.replace(UPSTREAM_COMMIT, "0" * 40), encoding="utf-8")
        expect_rejection(temp_root, "manifest-authority mutation")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    try:
        if args.self_test:
            self_test(root)
            print(json.dumps({"self_test": True, "passed": True}, sort_keys=True))
        else:
            print(json.dumps(verify_root(root), sort_keys=True))
    except (OSError, VerificationError) as exc:
        print(f"z7 named-reference table verification failed: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
