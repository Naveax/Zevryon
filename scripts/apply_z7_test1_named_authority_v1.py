#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RUNNER = ROOT / "scripts/z7_html5lib_tokenizer_test1_admitted_runner_v1.py"
DOC = ROOT / "docs/Z7_HTML5LIB_TOKENIZER_TEST1_ADMITTED_RUNNER_V1.md"


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected exactly one match, found {count}")
    return text.replace(old, new, 1)


runner = RUNNER.read_text(encoding="utf-8")
runner = replace_once(
    runner,
    "ADMITTED_EXECUTION_COUNT = 58\nUNSUPPORTED_EXECUTION_COUNT = 11",
    "ADMITTED_EXECUTION_COUNT = 68\nUNSUPPORTED_EXECUTION_COUNT = 1",
    "runner denominators",
)

anchor = '''        "Unquoted attribute at end of tag with final character of &, with tag followed by characters",\n        # Comment state-family surface.\n'''
addition = '''        "Unquoted attribute at end of tag with final character of &, with tag followed by characters",\n        # Bounded WHATWG named-character-reference surface.\n        "Unfinished entity",\n        "Entity with trailing semicolon (1)",\n        "Entity with trailing semicolon (2)",\n        "Entity without trailing semicolon (1)",\n        "Entity without trailing semicolon (2)",\n        "Partial entity match at end of file",\n        "Entity in attribute without semicolon ending in x",\n        "Entity in attribute without semicolon ending in 1",\n        "Entity in attribute without semicolon ending in i",\n        "Entity in attribute without semicolon",\n        # Comment state-family surface.\n'''
runner = replace_once(runner, anchor, addition, "named-reference allowlist insertion")
RUNNER.write_text(runner, encoding="utf-8", newline="\n")

DOC.write_text(
    '''# Z7 html5lib `test1.test` admitted runner v1

## Purpose

This slice executes the already-pinned `tokenizer/test1.test` corpus only across production tokenizer behavior that has separately reached an admitted canonical boundary. It is deliberately not a whole-fixture conformance claim.

The external corpus provenance remains pinned to:

- repository: `html5lib/html5lib-tests`
- commit: `224991ec10db04f056a89eed8b0bd8695fd2950e`
- upstream path: `tokenizer/test1.test`
- vendored path: `tests/fixtures/html5lib-tokenizer/test1.test`
- Git blob: `5323fbbeae2c6116aab14a716c8df1174e7870fb`
- byte size: `10006`
- SHA-256: `524fcfa4d561a14f0c4e72e0573549abe6341fd4dfb8e16bc2dcf59a608a7219`
- test objects / executions: `69 / 69`

## Frozen denominators

The runner freezes three separate counts:

- full pinned fixture executions: **69**
- admitted executions: **68**
- explicitly unsupported executions: **1**

A green admitted-runner result requires exactly:

- `passed = 68`
- `failed = 0`
- `unsupported = 1`

The one unsupported execution is not silently discarded and is not counted as passing. A green CTest therefore means only that the admitted 68-case surface matches the pinned external token/error stream.

## Admitted surface

The fixed allowlist is derived from production behavior already covered by component/canonical regressions:

- 6 DOCTYPE / bogus-comment declaration cases;
- 12 Data start-tag, end-tag, attribute and bounded recovery cases;
- 10 literal-ampersand / bounded numeric-character-reference cases;
- 10 bounded named-character-reference cases;
- 16 comment-state-family cases;
- 13 Script-data cases;
- 1 `plaintext element` token-stream case.

The named-reference promotion contributes exactly these ten pinned cases after the production named-character-reference slice is admitted:

- `Unfinished entity`;
- `Entity with trailing semicolon (1)`;
- `Entity with trailing semicolon (2)`;
- `Entity without trailing semicolon (1)`;
- `Entity without trailing semicolon (2)`;
- `Partial entity match at end of file`;
- `Entity in attribute without semicolon ending in x`;
- `Entity in attribute without semicolon ending in 1`;
- `Entity in attribute without semicolon ending in i`;
- `Entity in attribute without semicolon`.

These executions are observed through the ordinary production Data tokenizer and shared character-reference component. The runner performs no runner-side entity decoding. Longest-match resolution, legacy semicolonless behavior, attribute-context veto, ambiguous-ampersand fallback and named replacement payloads are therefore measured at the existing probe boundary.

The `plaintext element` case remains token-stream observation only. The tokenizer does not invent tree-builder feedback merely because it emitted an element token.

## Remaining unsupported execution

Exactly one pinned execution remains outside this admitted authority:

- `Non-ASCII character reference name`.

Its raw input contains non-ASCII bytes and therefore remains behind the separate input-preprocessing / non-ASCII tokenizer-location authority boundary. It must not be converted to a pass merely because named-character-reference production behavior is broader.

## Probe wire authority

The admitted runner consumes canonical production probe records without reconstructing tokens from node output:

- `C`: Character
- `E`: EndTag
- `S`: StartTag with self-closing flag and complete published attribute map
- `M`: Comment
- `D`: DOCTYPE with explicit public/system identifier presence and force-quirks
- `ERROR`: exact parse-error code, line and column
- `STATS`: common canonical token-stream counters

Character-reference output is observed through ordinary Character or StartTag attribute payloads in this existing wire format.

## Integrity checks

Before executing the probe, the runner:

1. requires the canonical manifest and canonical vendored fixture paths;
2. runs the aggregate tokenizer corpus provenance verifier;
3. requires the exact pinned `test1.test` Git blob, byte size, SHA-256, test count and execution count from the manifest;
4. requires all 69 descriptions to remain unique;
5. requires the fixed 68-description allowlist to match the pinned fixture exactly;
6. requires the unsupported denominator to remain exactly 1;
7. requires every admitted case to use an explicitly admitted initial state and ASCII input/`lastStartTag` surface.

The self-test independently checks the 68/1 partition and the probe wire parser.

## Explicit nonclaims

This runner does **not** claim that `test1.test` passes 69/69. The remaining raw non-ASCII execution stays unsupported until input preprocessing / non-ASCII tokenizer-location authority is separately admitted.

It also does not claim:

- full input-stream preprocessing or U+0000 raw-input replacement;
- full non-ASCII tokenizer/location authority;
- CDATA authority;
- tree-builder-driven tokenizer state selection;
- full WHATWG tokenizer conformance;
- `html_tokenizer_conformance` gate completion;
- `tree_builder_conformance` gate completion;
- Z7 completion.

Z7 remains `planned` until the configured canonical gates close with their actual required evidence.
''',
    encoding="utf-8",
    newline="\n",
)

print("prepared Z7 test1 named-reference authority promotion: 68 admitted / 1 unsupported")
