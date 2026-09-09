# Z7 bounded HTML tokenizer token stream v1

## Purpose

The frozen html5lib tokenizer corpus cannot be compared honestly against `ZVNSRC01` node-source output because the corpus defines tokenizer tokens, initial tokenizer state, appropriate-end-tag context and parse errors rather than DOM/node-source records.

This slice introduces a production-core token-event boundary that is separate from the tree builder and node-source writer. It covers the state surface exercised by the first pinned `contentModelFlags.test` authority without claiming full WHATWG tokenization.

## API

`tokenize_html_token_stream_v1()` accepts:

- a bounded input string;
- one explicit initial state: PLAINTEXT, RCDATA or RAWTEXT;
- `last_start_tag` for appropriate-end-tag matching; RCDATA/RAWTEXT require it to be non-empty;
- hard input/token byte bounds;
- a caller-owned `HtmlTokenizerV1Sink`;
- optional statistics and an explicit error channel.

Unknown enum values are rejected rather than falling back to another tokenizer state. The admitted `last_start_tag` surface is normalized ASCII-case-insensitively and remains bounded to the v1 tag-name byte subset.

The sink receives tokenizer events directly from the tokenizer state machine. Node-source records are not converted back into synthetic tokens.

The v1 token representation already names the standard token categories (`DOCTYPE`, `StartTag`, `EndTag`, `Comment`, `Character`) so later states can extend the same versioned event boundary. The admitted implementation in this slice emits `Character` and `EndTag` only because those are the token categories reached by the frozen initial-state fixture.

## Admitted text-state semantics

### PLAINTEXT

Every non-NUL input byte is character data through EOF. Apparent markup and character-reference spellings are not interpreted.

### RAWTEXT

Character data remains literal except for an ASCII-case-insensitive appropriate end tag based on `last_start_tag`.

An appropriate end tag followed by `>` emits the current coalesced Character token followed by an EndTag token and transitions to the admitted Data-state end-tag subset.

A matching name not followed by an appropriate delimiter is re-emitted as character data and the trailing byte is reconsumed. This is required for chained partial candidates such as `</xmp</xmp</xmp>`.

An appropriate end-tag candidate that ends exactly at EOF without a delimiter is re-emitted as character data.

If the tokenizer has entered the whitespace or slash portion of an appropriate end-tag token and then reaches EOF, the candidate is not re-emitted. The prior Character token is flushed and an `eof-in-tag` parse-error event is emitted at the EOF position.

### RCDATA

RCDATA uses the same appropriate-end-tag transition and additionally admits the bounded named-reference subset required by the current production/parser authority: `amp`, `lt`, `gt`, `quot` and `apos` with semicolons.

Unknown/general named references remain fail-closed. Full WHATWG character-reference behavior is later work.

## Data-state boundary

After an admitted RCDATA/RAWTEXT close, v1 supports ordinary named end tags with optional ASCII whitespace before `>`. This is sufficient to represent the `foo</xmp></baz>` transition in the frozen external authority.

General Data-state start tags, comments, DOCTYPEs, character references and malformed recovery are deliberately not approximated. They fail closed until admitted explicitly.

## Bounds and failure behavior

The API rejects configurations outside fixed implementation maxima:

- maximum input: 16 MiB;
- maximum coalesced token: 1 MiB.

Default caller bounds are 1 MiB input and 64 KiB token.

Character data is coalesced only up to the configured token limit. The sink consumes tokens incrementally, so the API does not retain the complete output stream internally.

Invalid initial-state enum values and missing RCDATA/RAWTEXT `last_start_tag` context fail before token/error events are emitted. Allocation failures in state/context setup and token execution are contained by the production API and reported through the explicit error channel.

## Parse-error authority

Parse errors are emitted through the same sink and carry one-based line/column positions. The pinned `contentModelFlags.test` EOF-in-tag cases therefore produce the expected line 1, column 10 authority rather than turning tokenizer parse errors into API failures.

V1 location accounting is byte-oriented between LF boundaries. That is sufficient for the currently pinned ASCII fixture; Unicode/input-preprocessing location authority is not claimed by this slice.

Unsupported semantics remain API failures. This distinction matters: a WHATWG parse error can still be a successful tokenizer execution whose emitted token/error stream is conformant.

## Focused tests

`html-tokenizer-token-stream-v1-tests` mirrors the semantic expectations of all 14 objects in the frozen `contentModelFlags.test` fixture and expands their initial-state matrix to exactly 24 executions:

- 2 PLAINTEXT executions;
- 20 paired RCDATA/RAWTEXT executions;
- 1 RAWTEXT entity-looking input;
- 1 RCDATA entity-decoding input.

Authority includes:

- ASCII-case-insensitive appropriate end tags;
- EOF partial-candidate recovery;
- whitespace/slash `eof-in-tag` behavior;
- left-angle-bracket candidate reconsumption;
- incorrect appropriate-end-tag names remaining Character data;
- chained partial candidates;
- transition back to Data end-tag handling;
- RAWTEXT literal ampersands;
- RCDATA `&lt;` decoding;
- token-byte hard-cap rejection;
- fail-closed NUL/input-preprocessing debt;
- invalid initial-state rejection;
- required RCDATA/RAWTEXT last-start-tag context.

These are C++ core tests. A later runner must still parse the vendored JSON fixture itself and compare the production event stream against the external expected outputs, including explicit pass/fail/unsupported accounting.

## Admission boundary

This slice establishes the production token-event surface needed by the frozen tokenizer corpus. It does not satisfy `html_tokenizer_conformance` by itself.

Still outstanding are the external corpus runner, general Data-state tokenization, start tags/attributes, comments, DOCTYPE, full named/numeric character references, script-data, CDATA, input-stream preprocessing, NUL replacement and broader tokenizer parse-error recovery.

The tree-builder remains a separate consumer/conformance boundary. Z7 therefore remains `planned`.
