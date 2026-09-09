# Z7 frozen html5lib tokenizer corpus authority

## Purpose

The configured Z7 milestone contains an `html_tokenizer_conformance` gate. Hand-written parser examples, deterministic property fuzzing and source-span equivalence are useful implementation authority, but they are not an external tokenizer conformance suite.

This slice establishes a frozen, license-preserving upstream corpus boundary before a production tokenizer-token adapter is admitted. It intentionally does **not** claim that Zevryon passes the vendored tokenizer cases.

## Upstream pin

Tokenizer data is pinned to:

- repository: `html5lib/html5lib-tests`;
- commit: `224991ec10db04f056a89eed8b0bd8695fd2950e`;
- vendored fixture: `tokenizer/contentModelFlags.test`;
- upstream fixture Git blob: `9cf7c8bd9e70dfbd0037726d6a840e67d3aa5e12`;
- fixture bytes: `3055`;
- fixture SHA-256: `77784a505a528950761cfb3c76617afade28b27c3be2a8c37dce3c3d8988391d`;
- fixture test objects: `14`;
- initial-state executions implied by those objects: `24`.

The upstream MIT license is vendored byte-for-byte at `third_party/html5lib-tests/LICENSE`. Its upstream Git blob is `8812371b41cfc6d2de3eac8c0b2a7a90f9b03428`, byte length is `1103` and SHA-256 is `ff512aac9ef231d504be5afaf4429005024e4b2aaf257be39524f37b8402aaf2`.

`config/z7_html5lib_tokenizer_corpus.json` is the machine-readable pin and explicitly carries `conformance_claim: false`.

## Why tokenizer and tree corpora are separate

At the pinned June 26, 2026 html5lib-tests commit, tree-construction tests were removed from that repository and moved to Web Platform Tests. The Z7 tokenizer and tree-builder gates therefore must not be represented as if one current html5lib repository snapshot still authoritatively owns both corpora.

This authority pins html5lib tokenizer data only. A later tree-builder authority must independently pin the applicable WPT parsing resources, license/provenance and importer contract.

## Corpus verifier

`scripts/z7_html5lib_tokenizer_corpus_verify.py` performs offline verification only. The v1 verifier contains the admitted upstream fixture path, vendored path, upstream Git blob, byte count, SHA-256, test count and execution count as code-level pins in addition to the manifest. A manifest edit therefore cannot silently redefine the authority.

Normal verification requires:

- exact manifest schema and authority;
- the admitted upstream repository and exact pinned commit;
- the exact admitted v1 fixture set and vendored path mapping;
- exact vendored license and fixture byte counts;
- exact SHA-256 values;
- Git blob SHA-1 recomputed from the vendored bytes using Git's `blob <size>\0<payload>` object encoding and compared with the pinned upstream blob identity;
- repository-relative paths that cannot escape the checkout;
- valid JSON with exactly `14` test objects;
- exactly `24` executions after expanding each object's `initialStates` list, with `Data state` used when omitted;
- required tokenizer test fields (`description`, `input`, `output`);
- known html5lib initial-state names with no duplicate state in one test object;
- structurally valid expected token arrays;
- structurally valid optional parse-error records with positive integer locations.

The verifier prints a machine-readable report containing `tests_verified: 14`, `executions_verified: 24`, `provenance_gate_passed: true` and `conformance_claim: false` only after every check succeeds.

Its `--self-test` path first validates the real pinned corpus, then proves that:

1. appending one byte to the fixture is rejected;
2. changing the upstream commit pin is rejected;
3. changing the upstream fixture Git-blob identity is rejected.

Both direct provenance verification and the tamper/drift self-test are wired into normal CTest when a Python 3 interpreter is available. No network access is required by either test.

## Tokenizer adapter boundary

The current production HTML v2 parser directly emits `ZVNSRC01` logical-node source records. html5lib tokenizer tests instead define complete tokenizer token streams and support explicit initial tokenizer states plus `lastStartTag` state.

A future conformance runner must therefore exercise an admitted production tokenizer-token boundary capable of representing at least:

- DOCTYPE tokens;
- start tags, attributes and self-closing flag;
- end tags;
- comments;
- coalesced character tokens;
- parse errors with deterministic locations where required by the corpus;
- explicit tokenizer initial state;
- appropriate-end-tag context through `lastStartTag`;
- input-stream preprocessing and `doubleEscaped` fixture decoding.

Node-source output must not be reverse-engineered into fake expected tokenizer tokens merely to obtain a green conformance number. Until that production token boundary exists and the external expected token streams are compared directly, this corpus is preparation/provenance authority only.

## Admission boundary

This slice makes the first external tokenizer fixture reproducible, path-locked and byte/Git-object tamper-evident. It does not satisfy `html_tokenizer_conformance`, does not satisfy `tree_builder_conformance`, and does not change Z7 from `planned`.

The next conformance implementation work is to add the production tokenizer token-sink boundary, consume the pinned fixture through that boundary, record explicit pass/fail/unsupported counts, and then expand the frozen tokenizer corpus without silently excluding failing cases.
