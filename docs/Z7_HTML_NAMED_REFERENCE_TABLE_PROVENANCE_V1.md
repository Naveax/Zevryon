# Z7 WHATWG Named Character Reference Table Provenance v1

## Scope

This slice pins the canonical WHATWG named-character-reference table and a deterministic Zevryon source representation. It is corpus/table provenance only. It does not connect the generated table to the production tokenizer.

The upstream authority is `whatwg/html-build` commit `283a3531a61106d07d9a7d9fb3e6f3b9bfd33d70`, path `entities/out/entities.json`, Git blob `557170b41f47a13a46ec695561eb5fe76da73bdb`.

The vendored JSON is frozen at:

- 145,897 bytes;
- SHA-256 `d741d877ac77c4194c4ad526b5b4a19aef8dfe411ab840a466891cdbb9f362e6`;
- 2,231 named-reference entries;
- 106 legacy names without a trailing semicolon;
- 93 two-scalar replacements;
- maximum entity-name spelling of 32 ASCII bytes after the leading ampersand.

The upstream `LICENSE` is vendored alongside the source. WHATWG states that source-code portions are available under the BSD 3-Clause terms; the complete upstream license text is retained verbatim in `third_party/whatwg-html-build/LICENSE`.

## Deterministic generated representation

`scripts/generate_html_named_character_references_v1.py` accepts only the exact pinned source bytes and validates every record before emitting:

- `src/html_named_character_references_v1.generated.hpp`;
- eight deterministic `.inc` partitions.

The header carries the upstream repository, commit, path, Git blob, SHA-256 and table cardinality. The split keeps individual generated files within a reviewable size while preserving one sorted 2,231-entry array. The first seven parts contain 279 entries each and the final part contains 278.

## Offline verification

`config/z7_html_named_reference_table.json` freezes the source identity and SHA-256 for every checked-in generated file.

`scripts/z7_html_named_reference_table_verify.py` runs without network access and requires:

1. exact v1 manifest authority;
2. vendored source byte size, SHA-256 and Git-blob SHA-1;
3. exact upstream license Git blob;
4. exact 2,231 / 106 / 93 / 32 table invariants;
5. the exact eight-part generated file set and frozen generated SHA-256 values;
6. a fresh temporary regeneration whose header and all eight parts are byte-identical to the checked-in outputs.

Its self-test proves rejection of source-byte drift, generated-output drift and coherent-looking manifest authority drift. Both verifier modes are registered as CTest tests.

## Next production slice

The separately reviewed production slice may consume this table in the shared character-reference component to implement WHATWG maximum-length matching, one- or two-scalar replacement, missing-semicolon diagnostics and the attribute-context legacy-name veto before `=` or ASCII alphanumeric input.

No tokenizer implementation is changed by this provenance slice.

## Explicit nonclaims

This slice does **not** claim:

- named-character-reference tokenizer behavior;
- `tokenizer/test1.test` 69/69;
- complete raw-input preprocessing or non-ASCII location authority;
- CDATA authority;
- full WHATWG tokenizer conformance;
- completion of `html_tokenizer_conformance`;
- tree-builder conformance;
- Z7 completion.

Z7 remains `planned`.
