# Z7 html5lib `test1.test` admitted runner v1

## Purpose

This slice executes the already-pinned `tokenizer/test1.test` corpus only across the production tokenizer behavior that has separately reached an admitted canonical boundary. It is deliberately not a whole-fixture conformance claim.

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
- admitted executions: **45**
- explicitly unsupported executions: **24**

A green admitted-runner result requires exactly:

- `passed = 45`
- `failed = 0`
- `unsupported = 24`

The 24 unsupported executions are not silently discarded and are not counted as passing. Therefore a green CTest result means only that the admitted 45-case surface matches the pinned external token/error stream.

## Admitted surface

The fixed allowlist is derived from behavior already covered by production component/canonical regressions:

- 6 DOCTYPE / bogus-comment declaration cases;
- 9 Data start-tag, end-tag and attribute cases;
- 16 comment-state-family cases;
- 13 Script-data cases;
- 1 `plaintext element` token-stream case.

The `plaintext element` case is token-stream observation only. The tokenizer emits the `plaintext` start tag and following character data; it does not invent tree-builder feedback or claim that the standalone tokenizer switched itself because an element token was emitted.

## Probe wire authority

The admitted runner consumes the canonical probe records without reconstructing tokens from node output:

- `C`: Character
- `E`: EndTag
- `S`: StartTag with self-closing flag and complete published attribute map
- `M`: Comment
- `D`: DOCTYPE with explicit public/system identifier presence and force-quirks
- `ERROR`: exact parse-error code, line and column
- `STATS`: common canonical token-stream counters

For html5lib DOCTYPE expectations, the fixture's final boolean is the token correctness bit while the probe exposes `force_quirks`; the runner compares them as logical inverses. Missing DOCTYPE identifiers remain distinct from present empty identifiers.

Start-tag attributes are compared as maps because the html5lib JSON fixture represents them as an object. The runner still rejects duplicate attributes in the published probe record rather than allowing a duplicate to disappear during normalization.

## Integrity checks

Before executing the probe, the runner:

1. requires the canonical manifest and canonical vendored fixture paths;
2. runs the aggregate tokenizer corpus provenance verifier;
3. requires the exact pinned `test1.test` Git blob, byte size, SHA-256, test count and execution count from the manifest;
4. requires all 69 descriptions to remain unique;
5. requires the fixed 45-description allowlist to match the pinned fixture exactly;
6. requires the unsupported denominator to remain exactly 24;
7. requires every admitted case to use an explicitly admitted initial state and ASCII input/`lastStartTag` surface.

The self-test independently checks the 45/24 partition and the extended StartTag/Comment/DOCTYPE/Character/EndTag probe wire parser.

## Explicit nonclaims

This runner does **not** claim that `test1.test` passes 69/69. In particular, the unsupported set still includes character-reference and related recovery/preprocessing surfaces not admitted by the current production tokenizer boundary.

It also does not claim:

- full input-stream preprocessing or U+0000 replacement;
- complete named or numeric character references;
- full non-ASCII tokenizer authority;
- CDATA authority;
- tree-builder-driven tokenizer state selection;
- full WHATWG tokenizer conformance;
- `html_tokenizer_conformance` gate completion;
- `tree_builder_conformance` gate completion;
- Z7 completion.

Z7 remains `planned` until the configured canonical gates close with their actual required evidence.
