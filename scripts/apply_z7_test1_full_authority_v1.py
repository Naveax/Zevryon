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
    "ADMITTED_EXECUTION_COUNT = 68\nUNSUPPORTED_EXECUTION_COUNT = 1",
    "ADMITTED_EXECUTION_COUNT = 69\nUNSUPPORTED_EXECUTION_COUNT = 0",
    "runner denominators",
)

anchor = '''        "Entity in attribute without semicolon",\n        # Comment state-family surface.\n'''
addition = '''        "Entity in attribute without semicolon",\n        # Literal non-ASCII Data following ambiguous-ampersand fallback.\n        "Non-ASCII character reference name",\n        # Comment state-family surface.\n'''
runner = replace_once(runner, anchor, addition, "final test1 allowlist insertion")

old_guard = '''        require(test.get("doubleEscaped", False) is False, f"admitted test {description!r} is doubleEscaped")\n        require(input_text.isascii() and last_start_tag.isascii(), f"admitted test {description!r} is non-ASCII")\n\n        label = f"test[{test_index}] {description} [{raw_state}]"\n'''
new_guard = '''        require(test.get("doubleEscaped", False) is False, f"admitted test {description!r} is doubleEscaped")\n        if input_text.isascii():\n            require(last_start_tag.isascii(), f"admitted test {description!r} has non-ASCII lastStartTag")\n        else:\n            require(\n                description == "Non-ASCII character reference name",\n                f"admitted test {description!r} uses an unpinned non-ASCII input surface",\n            )\n            require(\n                raw_state == "Data state" and last_start_tag == "" and input_text == "&¬;",\n                "pinned non-ASCII test1 authority shape drifted",\n            )\n\n        label = f"test[{test_index}] {description} [{raw_state}]"\n'''
runner = replace_once(runner, old_guard, new_guard, "non-ASCII authority guard")
runner = replace_once(
    runner,
    '        "full_fixture_pass_claim": False,\n',
    '        "full_fixture_pass_claim": (\n            admitted_surface_pass\n            and passed == RUNNER_EXECUTION_COUNT\n            and len(unsupported_descriptions) == 0\n        ),\n',
    "full fixture pass claim derivation",
)
RUNNER.write_text(runner, encoding="utf-8", newline="\n")

DOC.write_text(
    '''# Z7 html5lib `test1.test` admitted runner v1

## Purpose

This runner executes the pinned html5lib `tokenizer/test1.test` fixture only through production tokenizer behavior that has separately reached an admitted canonical boundary. With the bounded UTF-8 Data-text slice admitted, every execution in this one pinned fixture is now inside that production boundary.

This is a **fixture-complete authority claim for `test1.test` only**. It is deliberately not a claim of full WHATWG tokenizer conformance or of the wider html5lib tokenizer corpus.

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

The runner freezes:

- full pinned fixture executions: **69**
- admitted executions: **69**
- explicitly unsupported executions: **0**

A green result requires exactly:

- `passed = 69`
- `failed = 0`
- `unsupported = 0`

No execution in this pinned fixture is skipped, converted to a synthetic pass or decoded by runner-side entity logic. `full_fixture_pass_claim` is derived only when the admitted surface passes, all 69 executions pass, and the unsupported set is empty.

## Admitted surface

The 69-case fixed allowlist is derived from production behavior already covered by component/canonical regressions:

- 6 DOCTYPE / bogus-comment declaration cases;
- 12 Data start-tag, end-tag, attribute and bounded recovery cases;
- 10 literal-ampersand / bounded numeric-character-reference cases;
- 10 bounded named-character-reference cases;
- 1 literal non-ASCII Data case after ambiguous-ampersand fallback;
- 16 comment-state-family cases;
- 13 Script-data cases;
- 1 `plaintext element` token-stream case.

The final promoted case is exactly:

- description: `Non-ASCII character reference name`
- initial state: `Data state`
- `lastStartTag`: empty
- input: `&¬;` (UTF-8 bytes `26 c2 ac 3b`)

The name is inherited from html5lib. The production behavior does not treat U+00AC as an ASCII named-reference-name character. Character-reference consumption falls back to literal `&`; the admitted UTF-8 Data path then preserves U+00AC as ordinary character data, followed by `;`.

## Narrow non-ASCII authority guard

The runner does not remove its non-ASCII boundary wholesale. All admitted cases remain ASCII except the single pinned case above. If any other admitted input becomes non-ASCII, or if that case changes description, state, `lastStartTag` or exact input text, the runner fails before invoking the production probe.

This keeps the external authority tied to the exact production surface that was admitted rather than silently widening it to arbitrary Unicode markup or preprocessing behavior.

## Probe wire authority

The runner consumes canonical production probe records without reconstructing tokens from node output:

- `C`: Character
- `E`: EndTag
- `S`: StartTag with self-closing flag and complete published attribute map
- `M`: Comment
- `D`: DOCTYPE with explicit public/system identifier presence and force-quirks
- `ERROR`: exact parse-error code, line and column
- `STATS`: common canonical token-stream counters

Named/numeric reference decoding and UTF-8 Data handling are therefore measured at the same production boundary as the rest of the fixture.

## Integrity checks

Before executing the probe, the runner:

1. requires the canonical manifest and canonical vendored fixture paths;
2. runs the aggregate tokenizer corpus provenance verifier;
3. requires the exact pinned `test1.test` Git blob, byte size, SHA-256, test count and execution count from the manifest;
4. requires all 69 descriptions to remain unique;
5. requires the fixed 69-description allowlist to match the pinned fixture exactly;
6. requires the unsupported denominator to remain exactly 0;
7. requires every admitted case to use an explicitly admitted initial state;
8. requires ASCII input/`lastStartTag` for every case except the one exact pinned `&¬;` Data shape;
9. requires that exceptional shape to retain its exact description, state, empty `lastStartTag` and input text.

The built-in self-test independently checks the 69/0 partition and probe wire parser.

## Explicit nonclaims

A green runner now claims **69/69 only for this exact pinned `tokenizer/test1.test` fixture**. It does not claim:

- complete WHATWG input-stream preprocessing;
- raw U+0000 replacement or CR/LF normalization;
- arbitrary non-ASCII tag, attribute-name or raw attribute-value support;
- CDATA authority;
- tree-builder-driven tokenizer state selection;
- the wider pinned html5lib tokenizer corpus is fully executable;
- full WHATWG tokenizer conformance;
- `html_tokenizer_conformance` gate completion;
- `tree_builder_conformance` gate completion;
- Z7 completion.

Z7 remains `planned` until its configured canonical gates close with their actual required evidence.
''',
    encoding="utf-8",
    newline="\n",
)

print("prepared Z7 test1 full authority promotion: 69 admitted / 0 unsupported")
