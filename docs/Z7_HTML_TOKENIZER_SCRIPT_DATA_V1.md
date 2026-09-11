# Z7 HTML tokenizer Script-data v1

## Scope

`consume_html_script_data_v1()` is the bounded production component for the
Script-data state family. It emits through the shared `HtmlTokenizerV1Sink`
event boundary and either reaches ordinary EOF or stops immediately after a
successfully emitted appropriate end tag with an exact `next_offset`.

`tokenize_html_token_stream_v1()` now also exposes
`HtmlTokenizerV1InitialState::ScriptData`. The canonical entrypoint delegates the
Script-data prefix to the component and, when it reports
`transitioned_to_data=true`, continues tokenization of the unconsumed suffix
through the admitted bounded Data-stream composition.

The standalone component remains useful as the state-family authority; the
canonical entrypoint is the composition authority.

## Admitted Script-data state family

The implementation models the admitted ASCII behavior of:

- Script data less-than sign;
- Script data end tag open/name;
- Script data escape start / escape start dash;
- Script data escaped / escaped dash / escaped dash dash;
- Script data escaped less-than sign;
- Script data escaped end tag open/name;
- Script data double escape start;
- Script data double escaped / double escaped dash / double escaped dash dash;
- Script data double escaped less-than sign;
- Script data double escape end.

An appropriate end tag can leave Script data. A `</script>` spelling inside the
double-escaped path remains Character data until the double-escape state exits.
Appropriate end-tag comparison is ASCII case-insensitive and uses
`last_start_tag`.

## Canonical composition contract

For `HtmlTokenizerV1InitialState::ScriptData`:

1. the canonical entrypoint invokes `consume_html_script_data_v1()` on the full
   input;
2. accepted Script-data tokens/errors are published directly to the caller's
   shared sink;
3. Script-data counters are merged into `HtmlTokenizerV1Stats` with overflow
   checks;
4. if Script data reaches ordinary EOF, tokenization finishes there;
5. if an appropriate end tag transitions to Data, the returned `next_offset`
   is validated and only the remaining suffix is passed to
   `tokenize_html_data_stream_v1()`;
6. Data-stream parse-error locations are translated back to coordinates in the
   original full input;
7. Data-stream common token/error counters are merged into the same canonical
   stats object.

`HtmlTokenizerV1Stats::input_bytes` remains the size of the original complete
input. The canonical common counters include accepted events from both the
Script-data prefix and Data suffix; the suffix is not counted as a second input.

The Data suffix inherits the canonical `maximum_input_bytes` and
`maximum_token_bytes` bounds and uses the admitted Data-stream attribute bound
of 256.

## Regression authority

The standalone Script-data tests mirror the Script-data examples in pinned
`html5lib/html5lib-tests` commit
`224991ec10db04f056a89eed8b0bd8695fd2950e`, `tokenizer/test1.test` Git blob
`5323fbbeae2c6116aab14a716c8df1174e7870fb`.

They cover ordinary `<`, `<!`, `<!-`, escaped comment-like spellings,
script-looking content inside escaped/double-escaped text, dash-state variants,
appropriate closes, EOF behavior and fail-closed boundaries.

Canonical focused regressions additionally prove:

- `ScriptData` is a real public initial state, including the pinned-style
  double-escaped Character path;
- case-insensitive appropriate `</script>` emits the EndTag and continues into
  Data-state start/character/end-tag tokenization of the suffix;
- common token, Character-byte, EndTag and parse-error stats combine across the
  state transition;
- a delegated Data parse error after a multiline Script-data prefix is reported
  at its global original-input line/column;
- Script-data U+0000 is handled state-locally across ordinary, escaped and
  double-escaped Character-consuming states: one `unexpected-null-character`
  is emitted and UTF-8 U+FFFD is appended without disturbing state recovery.

The tokenizer probe accepts `SCRIPT_DATA` as an initial-state selector. Its
current wire protocol still exposes only Character and EndTag tokens because
that is the token surface needed by the currently admitted initial-state runner;
full Data-token probe serialization is intentionally a separate runner-expansion
change rather than an implicit protocol mutation.

## Deliberate fail-closed surfaces

Neither the standalone component nor canonical composition approximates:

- Data/markup/comment/DOCTYPE U+0000 recovery or complete input-stream preprocessing;
- general non-ASCII preprocessing/location authority;
- attributes on appropriate Script-data end tags;
- self-closing/trailing-solidus recovery on appropriate Script-data end tags;
- tree-builder-controlled selection of Script data from a `<script>` start tag;
- CDATA;
- Data-state character references, PUBLIC/SYSTEM doctype identifiers or broader
  recovery not already admitted by the Data-stream component.

The standalone component itself still stops at the Data transition boundary;
only the canonical entrypoint owns the subsequent Data suffix composition.

## External execution boundary

Adding `ScriptData` to the production enum/probe does not silently expand the
frozen `z7-html5lib-tokenizer-runner-v1` denominator. That runner remains the
admitted `contentModelFlags.test` authority of 14 objects / 24 executions until
a separate runner-expansion slice explicitly changes its corpus and reports the
new pass/fail/unsupported denominator.

The separately pinned `test1.test` corpus therefore remains provenance only
until such an execution slice is admitted.

## Conformance status

This is implementation/regression authority for bounded Script-data plus its
canonical transition into the admitted Data stream. It does **not** satisfy the
canonical `html_tokenizer_conformance` gate.

Complete Data-state NUL recovery, broader preprocessing/non-ASCII authority,
character-reference and malformed-markup recovery gaps, full tokenizer corpus
conformance, tree-builder-driven tokenizer transitions and
`tree_builder_conformance` remain outstanding. Z7 remains `planned`.
