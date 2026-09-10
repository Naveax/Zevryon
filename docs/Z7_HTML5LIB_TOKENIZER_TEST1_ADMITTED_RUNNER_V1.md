# Z7 html5lib `test1.test` admitted runner v1

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
- admitted executions: **58**
- explicitly unsupported executions: **11**

A green admitted-runner result requires exactly:

- `passed = 58`
- `failed = 0`
- `unsupported = 11`

The 11 unsupported executions are not silently discarded and are not counted as passing. A green CTest therefore means only that the admitted 58-case surface matches the pinned external token/error stream.

## Admitted surface

The fixed allowlist is derived from behavior already covered by production component/canonical regressions:

- 6 DOCTYPE / bogus-comment declaration cases;
- 12 Data start-tag, end-tag, attribute and bounded recovery cases;
- 10 literal-ampersand / bounded numeric-character-reference cases;
- 16 comment-state-family cases;
- 13 Script-data cases;
- 1 `plaintext element` token-stream case.

The recovery promotion contributes:

- `Empty end tag`;
- `Empty start tag`;
- `Open angled bracket in unquoted attribute value state`.

The numeric promotion contributes exactly these ten pinned cases:

- `Ampersand EOF`;
- `Ampersand ampersand EOF`;
- `Ampersand space EOF`;
- `Ampersand, number sign`;
- `Unfinished numeric entity`;
- `ASCII decimal entity`;
- `ASCII hexadecimal entity`;
- `Hexadecimal entity in attribute`;
- `Unquoted attribute ending in ampersand`;
- `Unquoted attribute at end of tag with final character of &, with tag followed by characters`.

These cases use the production Data tokenizer and shared character-reference component. The runner does not perform runner-side entity decoding.

The `plaintext element` case remains token-stream observation only. The tokenizer does not invent tree-builder feedback merely because it emitted an element token.

## Remaining unsupported executions

The remaining **11** pinned executions are deliberately excluded because they require named-character-reference matching or raw non-ASCII preprocessing authority that this production boundary has not admitted:

- `Unfinished entity`;
- `Entity with trailing semicolon (1)`;
- `Entity with trailing semicolon (2)`;
- `Entity without trailing semicolon (1)`;
- `Entity without trailing semicolon (2)`;
- `Partial entity match at end of file`;
- `Non-ASCII character reference name`;
- `Entity in attribute without semicolon ending in x`;
- `Entity in attribute without semicolon ending in 1`;
- `Entity in attribute without semicolon ending in i`;
- `Entity in attribute without semicolon`.

This explicit list is part of the authority boundary. Those executions must not become passes until their production semantics are separately admitted and measured.

## Probe wire authority

The admitted runner consumes canonical production probe records without reconstructing tokens from node output:

- `C`: Character
- `E`: EndTag
- `S`: StartTag with self-closing flag and complete published attribute map
- `M`: Comment
- `D`: DOCTYPE with explicit public/system identifier presence and force-quirks
- `ERROR`: exact parse-error code, line and column
- `STATS`: common canonical token-stream counters

Decoded numeric-reference output is observed through the ordinary Character or StartTag attribute payload in this existing wire format.

## Integrity checks

Before executing the probe, the runner:

1. requires the canonical manifest and canonical vendored fixture paths;
2. runs the aggregate tokenizer corpus provenance verifier;
3. requires the exact pinned `test1.test` Git blob, byte size, SHA-256, test count and execution count from the manifest;
4. requires all 69 descriptions to remain unique;
5. requires the fixed 58-description allowlist to match the pinned fixture exactly;
6. requires the unsupported denominator to remain exactly 11;
7. requires every admitted case to use an explicitly admitted initial state and ASCII input/`lastStartTag` surface.

The self-test independently checks the 58/11 partition and the probe wire parser.

## Explicit nonclaims

This runner does **not** claim that `test1.test` passes 69/69. Complete named-character-reference longest-match and ambiguous-ampersand rules remain outside this authority, as does raw non-ASCII character-reference-name preprocessing.

It also does not claim:

- full input-stream preprocessing or U+0000 raw-input replacement;
- complete named character references;
- full non-ASCII tokenizer/location authority;
- CDATA authority;
- tree-builder-driven tokenizer state selection;
- full WHATWG tokenizer conformance;
- `html_tokenizer_conformance` gate completion;
- `tree_builder_conformance` gate completion;
- Z7 completion.

Z7 remains `planned` until the configured canonical gates close with their actual required evidence.
