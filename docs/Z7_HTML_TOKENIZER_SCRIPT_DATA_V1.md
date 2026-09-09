# Z7 HTML tokenizer Script-data v1

## Scope

`consume_html_script_data_v1()` is a bounded production tokenizer component for
the Script data state family. It emits through the existing
`HtmlTokenizerV1Sink` token-event boundary and does not introduce a parallel
fixture-only tokenizer.

The component starts in Script data at `input[0]`. It either consumes through
ordinary EOF or stops immediately after an appropriate end tag, returning the
exact `next_offset` and `transitioned_to_data=true` so a later composition
layer can hand the remaining suffix to Data-state authority explicitly.

## Admitted state family

The implementation models the ASCII behavior of the WHATWG Script-data state
family needed by the current external tokenizer expansion:

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

This matters because Script data cannot be honestly represented as a single
"emit every byte as Character" loop. In particular, an appropriate end tag can
leave Script data, while a `</script>` spelling inside the double-escaped path
must remain character data until the double-escape state exits.

## Regression authority

The component tests mirror the Script-data examples in the pinned
`html5lib/html5lib-tests` commit
`224991ec10db04f056a89eed8b0bd8695fd2950e`, `tokenizer/test1.test` Git blob
`5323fbbeae2c6116aab14a716c8df1174e7870fb`.

Those examples cover ordinary `<`, `<!`, `<!-`, escaped comment-like spellings,
script-looking content inside escaped/double-escaped text, and the dash-state
variants around the double-escaped `script` sentinel.

Additional focused regressions cover:

- case-insensitive appropriate `</script>` recognition;
- optional ASCII whitespace before the closing `>`;
- exact stop position before the following Data suffix;
- appropriate end-tag whitespace followed by EOF, which reports `eof-in-tag`
  without emitting the incomplete end-tag token;
- nonappropriate incomplete end-tag candidates remaining literal text;
- an appropriate end tag while in escaped Script data;
- a double-escaped `</script>` spelling remaining character data;
- `eof-in-script-html-comment-like-text` in escaped/comment-like EOF;
- NUL/preprocessing and token hard-cap fail-closed behavior.

## Result and accounting contract

On ordinary EOF:

- `next_offset == input.size()`;
- `transitioned_to_data == false`.

On a successfully emitted appropriate end tag:

- the component flushes preceding Character data;
- emits the normalized EndTag token through the shared sink;
- sets `next_offset` to the first byte after the closing tag;
- sets `transitioned_to_data == true`;
- does not consume the following Data-state suffix.

`bytes_consumed` reports the physical prefix consumed by this component.
Accepted sink events remain published if a later byte reaches a fail-closed
condition; this component is streaming rather than transactional.

## Deliberate fail-closed surfaces

This v1 component does not approximate:

- U+0000 replacement or complete input-stream preprocessing;
- general non-ASCII preprocessing/location authority;
- attributes on appropriate Script-data end tags;
- self-closing/trailing-solidus recovery on appropriate end tags;
- the following Data-state suffix after an appropriate close;
- tree-builder-controlled selection of Script data from a `<script>` start tag;
- CDATA or character-reference behavior outside Script data.

The end-tag attribute/self-closing paths are left explicit rather than silently
reusing a partially admitted generic tag parser, because doing so would create
a broader recovery claim than this slice has evidence for.

## Composition status

This slice is intentionally standalone. It does not yet add `ScriptData` to the
public initial-state enum used by the frozen `contentModelFlags.test` runner,
and it does not alter that runner's canonical 24-execution authority.

A following composition slice can wire Script data into the canonical tokenizer
entrypoint/probe while preserving the existing frozen authority and adding a
separately pinned broader external fixture denominator.

## Conformance status

This component is implementation and regression authority for the bounded
Script-data state family only. It does **not** satisfy the canonical
`html_tokenizer_conformance` gate.

Complete character references, preprocessing, CDATA, broader Data recovery,
full `test1.test` execution, and `tree_builder_conformance` remain outstanding.
Z7 therefore remains `planned`.
