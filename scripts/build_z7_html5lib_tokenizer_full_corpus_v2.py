#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import pprint
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PINNED_COMMIT = "224991ec10db04f056a89eed8b0bd8695fd2950e"
UPSTREAM_REPOSITORY = "html5lib/html5lib-tests"
SCHEMA = "zevryon.z7.html5lib-tokenizer-corpus.v2"
AUTHORITY = "z7-html5lib-tokenizer-corpus-provenance-v2"
REPORT_SCHEMA = "zevryon.z7.html5lib-tokenizer-corpus-verification.v2"

EXPECTED_FIXTURES: dict[str, tuple[str, int]] = {
    "tokenizer/contentModelFlags.test": ("9cf7c8bd9e70dfbd0037726d6a840e67d3aa5e12", 3055),
    "tokenizer/domjs.test": ("1a0824d789b767792286c98e92843d388eb6b5ce", 13430),
    "tokenizer/entities.test": ("a6469cd0dc533d0c3b60d0ec8e8b3726524a98d7", 19147),
    "tokenizer/escapeFlag.test": ("d7d2c490bfd161681df4a846a454d588b94e3d19", 1378),
    "tokenizer/namedEntities.test": ("f74f5bff6d6513ca832da5dd12437d3f5d5861a5", 1128317),
    "tokenizer/numericEntities.test": ("085109b797acc6657dea2ccf7c29d31942cc7214", 49842),
    "tokenizer/pendingSpecChanges.test": ("191434f1b13532b03c1bc8519d6c4a1247ac2582", 162),
    "tokenizer/test1.test": ("5323fbbeae2c6116aab14a716c8df1174e7870fb", 10006),
    "tokenizer/test2.test": ("c29e4c315a66e9cb17f6629816331de4421af15a", 8647),
    "tokenizer/test3.test": ("901a581e35cd64fafec2db0b4d5b2390ec12534f", 349970),
    "tokenizer/test4.test": ("8963c7471184e53446afaa31e9cc7a74624a3812", 16339),
    "tokenizer/unicodeChars.test": ("49a8098528ea9771f42c619cb6a63d7bb4b6be86", 43771),
    "tokenizer/unicodeCharsProblematic.test": ("3ddb96c011b77706b80ff98f54bc4b1288bc8cf2", 1107),
    "tokenizer/xmlViolation.test": ("da6159e2ea7418684db258cd9fd7aad48f24af25", 442),
}

TEST_ARRAY_KEYS = {
    upstream_path: (
        "xmlViolationTests" if upstream_path == "tokenizer/xmlViolation.test" else "tests"
    )
    for upstream_path in EXPECTED_FIXTURES
}

LICENSE_PIN = {
    "vendored_path": "third_party/html5lib-tests/LICENSE",
    "git_blob": "8812371b41cfc6d2de3eac8c0b2a7a90f9b03428",
    "size_bytes": 1103,
    "sha256": "ff512aac9ef231d504be5afaf4429005024e4b2aaf257be39524f37b8402aaf2",
}


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(message)


def git_blob_sha1(data: bytes) -> str:
    return hashlib.sha1(f"blob {len(data)}\0".encode("ascii") + data).hexdigest()


def load_fixture_metadata(upstream: Path, upstream_path: str) -> dict[str, object]:
    source = upstream / upstream_path
    data = source.read_bytes()
    expected_blob, expected_size = EXPECTED_FIXTURES[upstream_path]
    require(len(data) == expected_size, f"{upstream_path}: byte-size drift")
    require(git_blob_sha1(data) == expected_blob, f"{upstream_path}: Git-blob drift")
    try:
        value = json.loads(data.decode("utf-8"))
    except Exception as exc:
        raise SystemExit(f"{upstream_path}: invalid UTF-8/JSON: {exc}") from exc

    require(isinstance(value, dict), f"{upstream_path}: top-level value must be object")
    test_array_key = TEST_ARRAY_KEYS[upstream_path]
    tests = value.get(test_array_key)
    require(
        isinstance(tests, list),
        f"{upstream_path}: missing pinned top-level {test_array_key} array",
    )

    execution_count = 0
    for index, test in enumerate(tests):
        require(isinstance(test, dict), f"{upstream_path} test[{index}]: not an object")
        states = test.get("initialStates", ["Data state"])
        require(
            isinstance(states, list) and states,
            f"{upstream_path} test[{index}]: invalid initialStates",
        )
        execution_count += len(states)

    vendored = f"tests/fixtures/html5lib-tokenizer/{Path(upstream_path).name}"
    destination = ROOT / vendored
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(data)
    return {
        "vendored_path": vendored,
        "git_blob": expected_blob,
        "size_bytes": expected_size,
        "sha256": hashlib.sha256(data).hexdigest(),
        "test_array_key": test_array_key,
        "test_count": len(tests),
        "execution_count": execution_count,
    }


def rewrite_verifier(fixtures: dict[str, dict[str, object]]) -> None:
    path = ROOT / "scripts/z7_html5lib_tokenizer_corpus_verify.py"
    text = path.read_text(encoding="utf-8")
    for old, new, label in (
        (
            'SCHEMA = "zevryon.z7.html5lib-tokenizer-corpus.v1"',
            f'SCHEMA = "{SCHEMA}"',
            "schema",
        ),
        (
            'AUTHORITY = "z7-html5lib-tokenizer-corpus-provenance-v1"',
            f'AUTHORITY = "{AUTHORITY}"',
            "authority",
        ),
        (
            'REPORT_SCHEMA = "zevryon.z7.html5lib-tokenizer-corpus-verification.v1"',
            f'REPORT_SCHEMA = "{REPORT_SCHEMA}"',
            "report schema",
        ),
        (
            "upstream_path is not admitted by v1 authority",
            "upstream_path is not admitted by v2 authority",
            "authority diagnostic",
        ),
    ):
        require(text.count(old) == 1, f"verifier {label}: expected one old marker")
        text = text.replace(old, new, 1)

    start = text.index("PINNED_FIXTURES = {")
    end_marker = "\n\nALLOWED_INITIAL_STATES = {"
    end = text.index(end_marker, start)
    literal = pprint.pformat(fixtures, width=110, sort_dicts=False)
    text = text[:start] + "PINNED_FIXTURES = " + literal + text[end:]

    old_array_logic = '''    tests = value.get("tests")
    require(isinstance(tests, list), f"{path} must contain top-level tests array")
'''
    new_array_logic = '''    manifest_test_array_key = entry.get("test_array_key")
    pinned_test_array_key = pinned["test_array_key"]
    require(
        manifest_test_array_key == pinned_test_array_key,
        f"{path} test-array-key pin drifted",
    )
    tests = value.get(pinned_test_array_key)
    require(
        isinstance(tests, list),
        f"{path} must contain top-level {pinned_test_array_key} array",
    )
'''
    require(
        text.count(old_array_logic) == 1,
        "verifier test-array parser marker drifted",
    )
    text = text.replace(old_array_logic, new_array_logic, 1)
    path.write_text(text, encoding="utf-8", newline="\n")


def write_manifest(fixtures: dict[str, dict[str, object]]) -> None:
    manifest = {
        "schema": SCHEMA,
        "authority": AUTHORITY,
        "conformance_claim": False,
        "upstream": {
            "repository": UPSTREAM_REPOSITORY,
            "commit": PINNED_COMMIT,
            "license": "MIT",
            "license_git_blob": LICENSE_PIN["git_blob"],
            "license_vendored_path": LICENSE_PIN["vendored_path"],
            "license_size_bytes": LICENSE_PIN["size_bytes"],
            "license_sha256": LICENSE_PIN["sha256"],
        },
        "files": [
            {"upstream_path": upstream_path, **metadata}
            for upstream_path, metadata in fixtures.items()
        ],
    }
    path = ROOT / "config/z7_html5lib_tokenizer_corpus.json"
    path.write_text(
        json.dumps(manifest, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
        newline="\n",
    )


def write_doc(fixtures: dict[str, dict[str, object]]) -> None:
    total_tests = sum(int(item["test_count"]) for item in fixtures.values())
    total_exec = sum(int(item["execution_count"]) for item in fixtures.values())
    rows = "\n".join(
        f"| `{path}` | `{meta['test_array_key']}` | {meta['size_bytes']} | `{meta['git_blob']}` | `{meta['sha256']}` | {meta['test_count']} | {meta['execution_count']} |"
        for path, meta in fixtures.items()
    )
    document = f'''# Z7 frozen html5lib tokenizer corpus authority v2

## Purpose

The configured Z7 milestone contains an `html_tokenizer_conformance` gate. This authority pins the complete `tokenizer/*.test` fixture set present at the selected html5lib-tests commit so later execution work has a complete, immutable denominator instead of a hand-picked green subset.

Provenance success is **not** tokenizer conformance. The manifest therefore keeps `conformance_claim: false`.

## Upstream pin

- repository: `html5lib/html5lib-tests`
- commit: `{PINNED_COMMIT}`
- license: MIT, vendored byte-for-byte at `{LICENSE_PIN['vendored_path']}`
- tokenizer fixture set: **{len(fixtures)} `.test` files**
- test objects: **{total_tests}**
- initial-state executions: **{total_exec}**

The one-shot builder requires the upstream checkout to be exactly the pinned commit and requires the complete `tokenizer/*.test` filename set, Git blob identities and byte sizes frozen in the builder before it copies anything.

`tokenizer/xmlViolation.test` is intentionally not normalized into the ordinary fixture shape: its upstream-defined `xmlViolationTests` top-level array name is separately pinned and verified. All other files use `tests`.

## Complete pinned fixture set

| Upstream fixture | Test array | Bytes | Git blob | SHA-256 | Tests | Executions |
| --- | --- | ---: | --- | --- | ---: | ---: |
{rows}

## Machine authority

`config/z7_html5lib_tokenizer_corpus.json` uses schema `{SCHEMA}` and authority `{AUTHORITY}`. `scripts/z7_html5lib_tokenizer_corpus_verify.py` independently embeds the complete file mapping, test-array key, Git blob, byte-size, SHA-256, test-count and execution-count pins. Editing only the manifest therefore cannot redefine the denominator.

The verifier additionally recomputes Git blob SHA-1 from vendored bytes, validates JSON/token structure and initial-state expansion, rejects path escape, and retains its tamper/commit/blob/type self-tests.

## Execution remains separate

The existing `contentModelFlags.test` and `test1.test` runners keep their own execution claims. Adding the other twelve fixtures to provenance does not convert them into passes and does not close `html_tokenizer_conformance`.

`xmlViolation.test` is provenance-visible but its expected output is defined by html5lib's historical XML-infoset coercion convention; pinning it is not a claim that the ordinary production tokenizer should blindly reproduce that convention.

The next execution pass must run the newly pinned fixtures through production tokenizer entrypoints, report exact pass/fail/unsupported denominators, and leave unsupported states/preprocessing/recovery visible rather than filtering them out.

## Tree-builder separation

At the pinned June 26, 2026 html5lib-tests commit, tree-construction tests had moved to Web Platform Tests. Z7 tree-builder provenance therefore remains separately pinned to WPT and is not mixed into this tokenizer authority.

## Nonclaims

This authority does not claim full WHATWG tokenizer conformance, `html_tokenizer_conformance`, `tree_builder_conformance`, chunk-size equivalence, parser fuzzing, bounded-large-document completion, or Z7 completion. Z7 remains `planned` until its configured gates close with execution evidence.
'''
    (ROOT / "docs/Z7_HTML5LIB_TOKENIZER_CORPUS.md").write_text(
        document,
        encoding="utf-8",
        newline="\n",
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--upstream", type=Path, required=True)
    args = parser.parse_args()
    upstream = args.upstream.resolve()
    head = subprocess.check_output(
        ["git", "-C", str(upstream), "rev-parse", "HEAD"],
        text=True,
    ).strip()
    require(head == PINNED_COMMIT, f"upstream checkout is {head}, expected {PINNED_COMMIT}")

    actual = {
        f"tokenizer/{path.name}"
        for path in (upstream / "tokenizer").glob("*.test")
    }
    require(
        actual == set(EXPECTED_FIXTURES),
        f"tokenizer fixture set drift: actual={sorted(actual)!r}",
    )

    license_data = (upstream / "LICENSE").read_bytes()
    require(len(license_data) == LICENSE_PIN["size_bytes"], "license byte-size drift")
    require(git_blob_sha1(license_data) == LICENSE_PIN["git_blob"], "license Git-blob drift")
    require(
        hashlib.sha256(license_data).hexdigest() == LICENSE_PIN["sha256"],
        "license SHA-256 drift",
    )
    require(
        (ROOT / LICENSE_PIN["vendored_path"]).read_bytes() == license_data,
        "vendored license differs from pinned upstream",
    )

    fixtures = {
        upstream_path: load_fixture_metadata(upstream, upstream_path)
        for upstream_path in EXPECTED_FIXTURES
    }
    write_manifest(fixtures)
    rewrite_verifier(fixtures)
    write_doc(fixtures)

    total_tests = sum(int(item["test_count"]) for item in fixtures.values())
    total_exec = sum(int(item["execution_count"]) for item in fixtures.values())
    print(
        json.dumps(
            {"files": len(fixtures), "tests": total_tests, "executions": total_exec},
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
