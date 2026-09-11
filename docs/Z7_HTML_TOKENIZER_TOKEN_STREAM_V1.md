# Z7 bounded HTML tokenizer token stream v1

## Purpose

The frozen html5lib tokenizer corpus cannot be compared honestly against `ZVNSRC01` node-source output because the corpus defines tokenizer tokens, initial tokenizer state, appropriate-end-tag context and parse errors rather than DOM/node-source records.

This slice introduces a production-core token-event boundary that is separate from the tree builder and node-source writer. It covers the state surface exercised by the admitted tokenizer authorities without claiming full WHATWG tokenization.

## API

`tokenize_html_token_stream_v1()` accepts:

- a bounded input string;
- one explicit admitted initial state;
- optional `last_start_tag` authority for appropriate-end-tag matching; an empty value means no RCDATA/RAWTEXT end-tag candidate can be appropriate;
- hard input/token byte bounds;
- a caller-owned `HtmlTokenizerV1Sink`;
- optional statistics and an explicit error channel.

Unknown enum values are rejected rather than falling back to another tokenizer state. The admitted non-empty `last_start_tag` surface is normalized ASCII-case-insensitively and remains bounded to the v1 tag-name byte subset.

The sink receives tokenizer events directly from the tokenizer state machine. Node-source records are not converted back into synthetic tokens.

The v1 token representation names the standard token categories (`DOCTYPE`, `StartTag`, `EndTag`, `Comment`, `Character`) so composed tokenizer states can share the same versioned event boundary.

## Admitted text-state semantics

### PLAINTEXT

Every non-NUL input byte is character data through EOF. Apparent markup and character-reference spellings are not interpreted.

### RAWTEXT

Character data remains literal except for an ASCII-case-insensitive appropriate end tag when non-empty `last_start_tag` authority exists. With an empty `last_start_tag`, no end-tag candidate is appropriate and `</...` spellings remain literal character data.

An appropriate end tag followed by `>` emits the current coalesced Character token followed by an EndTag token and transitions to the admitted Data-state path.

A matching name not followed by an appropriate delimiter is re-emitted as character data and the trailing byte is reconsumed. This is required for chained partial candidates such as `</xmp</xmp</xmp>`.

An appropriate end-tag candidate that ends exactly at EOF without a delimiter is re-emitted as character data.

If the tokenizer has entered the whitespace or slash portion of an appropriate end-tag token and then reaches EOF, the candidate is not re-emitted. The prior Character token is flushed and an `eof-in-tag` parse-error event is emitted at the EOF position.

### RCDATA

RCDATA uses the same appropriate-end-tag transition and routes character references through the shared bounded canonical character-reference consumer. This admits the canonical named/numeric reference table already used by the Data context instead of maintaining a second local five-name subset.

Character-reference parse errors are forwarded through the token-stream sink and included in token-stream statistics. RAWTEXT continues to preserve entity-looking bytes literally.

### Empty last-start-tag authority

RCDATA and RAWTEXT accept an empty `last_start_tag`. In this authority shape every `</...` spelling is reconsumed as text, preventing the tokenizer from manufacturing an empty-name EndTag token. RCDATA character references remain active; RAWTEXT remains literal.

## Data-state boundary

After an admitted RCDATA/RAWTEXT close, execution transitions into the composed Data-state tokenizer path. Data-state production support is versioned independently and may still fail closed at explicitly unadmitted recovery boundaries.

## Bounds and failure behavior

The API rejects configurations outside fixed implementation maxima:

- maximum input: 16 MiB;
- maximum coalesced token: 1 MiB.

Default caller bounds are 1 MiB input and 64 KiB token.

Character data is coalesced only up to the configured token limit. The sink consumes tokens incrementally, so the API does not retain the complete output stream internally.

Invalid initial-state enum values fail before token/error events are emitted. Allocation failures in state/context setup and token execution are contained by the production API and reported through the explicit error channel.

Raw NUL is state-local rather than globally admitted. PLAINTEXT, RCDATA and RAWTEXT emit `unexpected-null-character` and append U+FFFD for U+0000. CDATA retains its separately admitted literal-NUL authority. Data and Script-data NUL behavior remain outside this slice and stay behind the corpus NUL authority boundary.

## Parse-error authority

Parse errors are emitted through the same sink and carry one-based line/column positions. The pinned `contentModelFlags.test` EOF-in-tag cases therefore produce the expected line 1, column 10 authority rather than turning tokenizer parse errors into API failures.

RCDATA and RAWTEXT now emit `control-character-in-input-stream` for the admitted C0/DEL input-control set while preserving the input byte as character data. This closes the two U+000B regressions exposed when empty `last_start_tag` executions first became runnable.

V1 location accounting is byte-oriented between LF boundaries. Broader Unicode/input-preprocessing location authority is not claimed by this slice.

Unsupported semantics remain API failures. This distinction matters: a WHATWG parse error can still be a successful tokenizer execution whose emitted token/error stream is conformant.

## Focused tests

`html-tokenizer-token-stream-v1-tests` retains the pinned content-model semantics and adds direct regressions for the empty-context text-state authority.

Authority includes:

- ASCII-case-insensitive appropriate end tags;
- EOF partial-candidate recovery;
- whitespace/slash `eof-in-tag` behavior;
- left-angle-bracket candidate reconsumption;
- incorrect appropriate-end-tag names remaining Character data;
- chained partial candidates;
- transition back to Data handling;
- RAWTEXT literal ampersands;
- RCDATA canonical named and numeric reference decoding;
- empty-context RCDATA/RAWTEXT literal end-tag spellings;
- RCDATA/RAWTEXT `control-character-in-input-stream` diagnostics for U+000B;
- token-byte hard-cap rejection;
- fail-closed NUL/input-preprocessing debt;
- invalid initial-state rejection.

These are C++ core tests. Full admission still depends on the vendored html5lib corpus census comparing production token/error streams against the external expected outputs with explicit pass/fail/unsupported accounting.

## Admission boundary

This slice advances the production token-event surface used by the frozen tokenizer corpus. It does not satisfy `html_tokenizer_conformance` by itself.

Still outstanding are explicitly unsupported corpus surfaces such as Data/Script-data NUL handling, broader non-ASCII preprocessing/location authority, nullable DOCTYPE-name probe-wire representation and remaining malformed Data-tag/DOCTYPE/comment recovery buckets.

The tree-builder remains a separate consumer/conformance boundary. Z7 therefore remains unchanged.