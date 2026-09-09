# Z7 frozen html5lib tokenizer corpus authority

## Purpose

The configured Z7 milestone contains an `html_tokenizer_conformance` gate. Hand-written parser examples, deterministic property fuzzing and source-span equivalence are useful implementation authority, but they are not an external tokenizer conformance suite.

This authority freezes license-preserving upstream tokenizer fixtures independently from the code that executes them. Vendoring a fixture is provenance authority, not a claim that Zevryon passes it.

## Upstream pin

Tokenizer data is pinned to:

- repository: `html5lib/html5lib-tests`;
- commit: `224991ec10db04f056a89eed8b0bd8695fd2950e`.

The v1 admitted fixture set is:

### `tokenizer/contentModelFlags.test`

- vendored path: `tests/fixtures/html5lib-tokenizer/contentModelFlags.test`;
- upstream Git blob: `9cf7c8bd9e70dfbd0037726d6a840e67d3aa5e12`;
- bytes: `3055`;
- SHA-256: `77784a505a528950761cfb3c76617afade28b27c3be2a8c37dce3c3d8988391d`;
- test objects: `14`;
- initial-state executions: `24`.

### `tokenizer/test1.test`

- vendored path: `tests/fixtures/html5lib-tokenizer/test1.test`;
- upstream Git blob: `5323fbbeae2c6116aab14a716c8df1174e7870fb`;
- bytes: `10006`;
- SHA-256: `524fcfa4d561a14f0c4e72e0573549abe6341fd4dfb8e16bc2dcf59a608a7219`;
- test objects: `69`;
- initial-state executions: `69`.

Across both files the provenance authority therefore freezes exactly `83` test objects and `93` initial-state executions.

The upstream MIT license is vendored byte-for-byte at `third_party/html5lib-tests/LICENSE`. Its upstream Git blob is `8812371b41cfc6d2de3eac8c0b2a7a90f9b03428`, byte length is `1103` and SHA-256 is `ff512aac9ef231d504be5afaf4429005024e4b2aaf257be39524f37b8402aaf2`.

`config/z7_html5lib_tokenizer_corpus.json` is the machine-readable pin and explicitly carries `conformance_claim: false`.

## Why tokenizer and tree corpora are separate

At the pinned June 26, 2026 html5lib-tests commit, tree-construction tests were removed from that repository and moved to Web Platform Tests. The Z7 tokenizer and tree-builder gates therefore must not be represented as if one current html5lib repository snapshot authoritatively owns both corpora.

Tokenizer provenance remains under this html5lib authority. Tree-builder provenance is pinned separately from WPT.

## Corpus verifier

`scripts/z7_html5lib_tokenizer_corpus_verify.py` performs offline verification only. The verifier contains the admitted upstream fixture paths, vendored mappings, Git blobs, byte counts, SHA-256 digests, test counts and execution counts as code-level pins in addition to the manifest. Editing the manifest alone therefore cannot redefine the authority.

Normal verification requires:

- exact manifest schema and authority;
- the exact upstream repository and pinned commit;
- the exact two-file v1 fixture set and vendored path mappings;
- exact license and fixture byte counts and SHA-256 values;
- Git blob SHA-1 recomputed from each vendored payload using Git's `blob <size>\0<payload>` encoding;
- repository-relative paths that cannot escape the checkout;
- valid JSON containing exactly `83` test objects in total;
- exactly `93` executions after expanding each object's `initialStates`, with `Data state` used when omitted;
- required tokenizer fields (`description`, `input`, `output`);
- known initial-state names with no duplicates inside one test object;
- structurally valid expected token arrays and optional parse-error records.

Token-kind validation explicitly requires a string before set membership, so malformed/unhashable JSON values produce controlled `VerificationError` rejection rather than leaking a Python `TypeError`.

A successful verifier report contains `files_verified: 2`, `tests_verified: 83`, `executions_verified: 93`, `provenance_gate_passed: true` and `conformance_claim: false`.

The `--self-test` path validates the real pinned corpus and proves rejection of:

1. a one-byte fixture tamper;
2. upstream commit drift;
3. upstream Git-blob identity drift;
4. a malformed non-string tokenizer token kind.

Both direct provenance verification and the self-test run under normal CTest when Python 3 is available. Neither requires network access.

## Execution authority remains separate

The existing frozen `z7-html5lib-tokenizer-runner-v1` still executes only `contentModelFlags.test`: `14` objects expanded to exactly `24` initial-state executions. Adding `test1.test` to provenance does **not** silently increase that runner denominator and does not convert unexecuted cases into passes.

The production tokenizer work now has an admitted shared token-event schema plus bounded PLAINTEXT/RCDATA/RAWTEXT, Data tags, markup declarations, Data-stream composition and a standalone Script-data component. That implementation progress does not change the external execution denominator by itself.

A separate runner-expansion slice must deliberately bind `test1.test` to production tokenizer entrypoints and report exact pass/fail/unsupported counts. Unsupported character-reference, preprocessing, recovery or state-transition surfaces must remain visible rather than being filtered out to manufacture a green percentage.

Node-source output must never be reverse-engineered into fake tokenizer tokens merely to satisfy the corpus.

## Admission boundary

This slice expands reproducible, path-locked and byte/Git-object-tamper-evident tokenizer provenance from one fixture to two. It does not satisfy `html_tokenizer_conformance`, does not satisfy `tree_builder_conformance`, and does not change Z7 from `planned`.

The next execution work is to compose Script data into the canonical tokenizer entrypoint/probe and add an explicit `test1.test` runner authority without altering the frozen 24-execution `contentModelFlags.test` authority.
