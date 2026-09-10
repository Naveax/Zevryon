# Z7 canonical Data tokenizer state v1

## Scope

This slice exposes the bounded Data-stream coordinator through the canonical production token-event boundary:

- `HtmlTokenizerV1InitialState::Data`
- `tokenize_html_token_stream_v1()`

The canonical entrypoint delegates Data input to `tokenize_html_data_stream_v1()` with the configured input/token byte bounds and the admitted `maximum_attributes = 256` bound. `last_start_tag` is intentionally not validated for a Data initial state because Data tokenization does not consume it.

This is composition authority, not a second Data tokenizer implementation.

The delegated Data surface now includes literal ampersand fallback plus bounded decimal/hex numeric character references in Data and attribute contexts. Numeric decoding may yield multi-byte UTF-8 output while raw-input preprocessing/location authority remains ASCII-only.

## Event and accounting semantics

The Data-stream remains a streaming, non-transactional producer. Events accepted by the downstream sink before a later fail-closed condition remain published.

The canonical boundary merges Data-stream common counters whether the delegated call succeeds or fails:

- total emitted tokens;
- Character-token count;
- Character bytes;
- EndTag count;
- parse-error count.

Character-reference parse errors are emitted through the same sink and therefore flow into this common accounting. `input_bytes` remains the byte length of the original canonical input and is not added a second time from delegated stats.

## Canonical regression authority

`html-tokenizer-data-canonical-v1-tests` covers ordered production Data tokens, recoverable parse-error coordinates, success/failure common accounting, accepted-prefix preservation and Data-state independence from unused `last_start_tag`.

Component-level Data-tag, markup-declaration, Data-stream, tag-recovery and numeric-character-reference tests own the narrower invariants beneath this canonical boundary.

## Probe protocol

The production-linked tokenizer probe accepts `DATA` and exposes lossless Character/EndTag/StartTag/Comment/DOCTYPE records. Existing Character/EndTag records remain byte-for-byte stable for the frozen `contentModelFlags.test` authority.

Decoded numeric-reference output is visible naturally through Character data or StartTag attribute values as UTF-8 hex in the existing probe protocol; no runner-only decoding path is introduced.

## External corpus authority remains separate

The frozen `contentModelFlags.test` runner remains 14 objects / 24 executions. The pinned `tokenizer/test1.test` runner separately records admitted/pass/fail/unsupported accounting. Production capability broadening does not silently rewrite that historical denominator; supported cases are promoted only by a dedicated runner-authority slice.

## Explicit nonclaims

This slice does **not** claim:

- complete named-character-reference handling or the full named table;
- complete input-stream preprocessing or U+0000 raw-input replacement;
- general non-ASCII raw-input preprocessing/location authority;
- complete DOCTYPE PUBLIC/SYSTEM support;
- complete recovery behavior;
- CDATA authority;
- full `tokenizer/test1.test` success;
- `html_tokenizer_conformance` gate closure;
- `tree_builder_conformance` gate closure;
- Z7 completion.

Z7 therefore remains `planned`.
