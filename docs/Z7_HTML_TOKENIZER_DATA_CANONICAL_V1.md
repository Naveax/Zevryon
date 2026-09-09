# Z7 canonical Data tokenizer state v1

## Scope

This slice exposes the already-admitted bounded Data-stream coordinator through the canonical production token-event boundary:

- `HtmlTokenizerV1InitialState::Data`
- `tokenize_html_token_stream_v1()`

The canonical entrypoint delegates Data input to `tokenize_html_data_stream_v1()` with the same configured input/token byte bounds and the admitted `maximum_attributes = 256` bound. `last_start_tag` is intentionally not validated for a Data initial state because Data tokenization does not consume it.

This is composition authority, not a second Data tokenizer implementation.

## Event and accounting semantics

The Data-stream remains a streaming, non-transactional producer. Events accepted by the downstream sink before a later fail-closed condition remain published.

The canonical boundary therefore merges Data-stream common counters whether the delegated call succeeds or fails:

- total emitted tokens;
- Character-token count;
- Character bytes;
- EndTag count;
- parse-error count.

`input_bytes` remains the byte length of the original canonical input and is not added a second time from the delegated Data-stream stats.

The focused regression authority includes a valid Comment followed by an unsupported PUBLIC DOCTYPE to prove that an accepted prefix event and its common accounting survive a later fail-closed result.

## Canonical regression authority

`html-tokenizer-data-canonical-v1-tests` covers:

1. ordered Character, Comment, StartTag+attribute, Character, EndTag, DOCTYPE and Character output through `HtmlTokenizerV1InitialState::Data`;
2. recoverable Data parse-error code and global line/column preservation;
3. common canonical stats for successful and fail-closed delegated execution;
4. preservation of already accepted prefix events on a later unsupported PUBLIC/SYSTEM DOCTYPE surface;
5. Data-state independence from the unused `last_start_tag` argument.

Component-level Data-tag, markup-declaration and Data-stream tests remain separate and continue to own their narrower invariants.

## Probe protocol

The production-linked tokenizer probe now accepts `DATA` in addition to the previously admitted state selectors.

The legacy records used by the frozen `contentModelFlags.test` runner remain byte-for-byte unchanged:

- `TOKEN\tC\t<data-hex>` for Character;
- `TOKEN\tE\t<name-hex>` for EndTag.

Additional lossless records are available for a later external-corpus adapter:

- `S`: StartTag name, self-closing flag, exact attribute count, then ordered name/value hex pairs;
- `M`: Comment data;
- `D`: DOCTYPE name, explicit public/system identifier presence bits and values, and force-quirks.

The presence bits are required because html5lib expectations distinguish a missing DOCTYPE identifier from an empty identifier.

## Frozen external runner remains unchanged in scope

The currently admitted `z7-html5lib-tokenizer-runner-v1` remains frozen to `contentModelFlags.test` at exactly 14 test objects / 24 initial-state executions. The separately pinned `tokenizer/test1.test` corpus is provenance authority only at this stage.

A later runner slice must explicitly parse the new `S/M/D` records, map default/Data initial-state semantics, compare full external token structures, and report unsupported cases separately. Unsupported cases must never be converted into passes merely to improve a conformance percentage.

## Explicit nonclaims

This slice does **not** claim:

- execution or passing of `tokenizer/test1.test`;
- complete named or numeric character-reference handling;
- complete input-stream preprocessing or U+0000 replacement;
- general non-ASCII preprocessing/location authority;
- complete DOCTYPE PUBLIC/SYSTEM support;
- complete recovery behavior;
- CDATA authority;
- `html_tokenizer_conformance` gate closure;
- `tree_builder_conformance` gate closure;
- Z7 completion.

Z7 therefore remains `planned`.
