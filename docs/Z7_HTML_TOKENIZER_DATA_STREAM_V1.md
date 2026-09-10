# Z7 HTML tokenizer Data stream v1

## Scope

`tokenize_html_data_stream_v1()` composes the separately admitted bounded
Data-tag tokenizer and markup-declaration tokenizer behind the shared
`HtmlTokenizerV1Sink` production event boundary.

The purpose of this slice is composition authority, not a new parallel token
model. StartTag, EndTag, Character, Comment and DOCTYPE events continue to use
the same shared token schema.

## Admitted composition behavior

The coordinator currently provides:

- ordered Data-tag and `<!...>` markup-declaration delivery through one sink;
- quoted attribute isolation so declaration-looking bytes inside a tag are not
  routed to the markup helper;
- literal ampersand fallback and bounded decimal/hex numeric character
  references inherited from the Data-tag component in Data and attribute
  contexts;
- delegated parse-error translation back to original-input line/column
  coordinates;
- incremental source-position advancement across Data and markup units;
- bounded input, token and attribute configuration inherited by delegates;
- aggregate token, attribute, parse-error and successfully-consumed-unit
  accounting;
- fail-closed propagation when a delegated tokenizer reaches an unsupported or
  bounded surface.

Numeric references may produce UTF-8 replacement bytes while raw-input
preprocessing/location authority remains ASCII-only. Those decoded bytes remain
subject to the existing token and attribute byte bounds.

## Source-position complexity hardening

An earlier preparation version recomputed each Data segment's base position by
rescanning the input prefix from byte zero. A declaration-heavy input could
therefore turn position accounting into quadratic work even though the input
size itself remained bounded.

The admitted implementation instead carries a monotonic `SourcePosition`
cursor. Each successfully consumed byte advances that cursor exactly as the
stream progresses. Delegates emit local coordinates through an offset sink,
which translates them to the current original-input position. Already consumed
prefixes are not rescanned for later segments.

The regression suite includes 2,048 comment declarations separated by newlines
followed by a Data-state diagnostic. The final diagnostic must remain at the
exact global line/column while all declaration and segment accounting stays
consistent. This is a deterministic complexity-structure regression, not a
wall-clock performance certification.

## Streaming and failure semantics

The API is deliberately streaming and non-transactional. If the caller's sink
accepts tokens or parse errors and a later byte reaches a fail-closed surface,
previously accepted events are not rolled back.

`data_segments_consumed` and `markup_declarations_consumed` count only units
whose delegated parse completed successfully. Token/error counters still
reflect events that were actually accepted before a later failure.

## Still fail closed

This composition does not admit or approximate:

- complete named character references and their longest-match / ambiguous-
  ampersand rules;
- NUL replacement and complete input-stream preprocessing;
- general non-ASCII raw-input tokenizer/location authority;
- Script data state, which is composed at the canonical token-stream layer;
- CDATA section state;
- DOCTYPE PUBLIC/SYSTEM identifiers;
- broad malformed-tag or malformed-DOCTYPE recovery beyond the already
  admitted component slices;
- tree-builder-driven tokenizer state transitions.

In particular, the Data tokenizer does not silently infer tree-builder actions
from element names merely to match an external fixture.

## Conformance status

This slice composes admitted production tokenizer pieces and adds
composition-specific regression authority. It does **not** satisfy the
canonical `html_tokenizer_conformance` gate.

The frozen `contentModelFlags.test` authority and admitted `test1.test` runner
remain separate measurement surfaces. Broader html5lib coverage still requires
complete named references, preprocessing, CDATA and further recovery behavior.

`tree_builder_conformance` also remains independently outstanding. Z7 therefore
remains `planned`.
