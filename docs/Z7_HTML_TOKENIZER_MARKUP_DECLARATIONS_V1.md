# Z7 HTML tokenizer markup declarations v1

## Purpose

This slice adds a bounded production tokenizer component for the first
Data-state markup-declaration behaviors required by the frozen html5lib
`tokenizer/test1.test` authority. It uses the same `HtmlTokenizerV1Sink` and
`HtmlTokenizerV1Token` event schema as the already admitted tokenizer surfaces;
it does not derive tokens from DOM/node-source output.

The upstream authority is `html5lib/html5lib-tests` at commit
`224991ec10db04f056a89eed8b0bd8695fd2950e`. The exact upstream
`tokenizer/test1.test` Git blob observed for this work is
`5323fbbeae2c6116aab14a716c8df1174e7870fb`.

This branch does not yet vendor or claim the complete `test1.test` fixture.
The blob identity is an implementation reference only until a later corpus
provenance slice pins byte size, SHA-256, object count and execution count.

## Admitted component surface

`consume_html_markup_declaration_v1()` consumes exactly one declaration whose
opening byte is `<` followed by `!`. The v1 component admits:

- case-insensitive `<!DOCTYPE ...>` recognition;
- ASCII-case-normalized simple DOCTYPE names;
- null public/system identifiers for the simple-DOCTYPE subset;
- the html5lib correctness/force-quirks mapping (`correctness=true` means
  `force_quirks=false`);
- EOF-after-DOCTYPE-name behavior with `eof-in-doctype` and force-quirks set;
- standard `<!-- ... -->` comment termination;
- abrupt empty-comment closes (`<!-->` and `<!--->`);
- EOF-in-comment behavior;
- nested-comment diagnostics for the frozen `test1.test` cases;
- bogus-comment recovery for incorrectly opened declarations such as `<!DOC>`;
- bounded token bytes and explicit sink/stat accounting.

The tests mirror the relevant frozen `test1.test` examples, including exact
one-based parse-error locations.

## Deliberate fail-closed boundaries

The component does not approximate:

- DOCTYPE PUBLIC or SYSTEM identifiers;
- broad malformed-DOCTYPE recovery;
- NUL replacement/input preprocessing;
- non-ASCII preprocessing/location authority;
- script-data or CDATA states;
- named or numeric character references.

These remain separate admissions because silently accepting an approximate
state machine would make later external-conformance numbers meaningless.

## Composition boundary

This PR-stage component is intentionally not claimed as the full Data-state
stream. The existing `tokenize_html_data_tags_v1()` authority remains unchanged
and still rejects markup declarations. A following composition slice must route
Data-state `<!...>` transitions through this component while preserving token
ordering, global parse-error locations, hard-cap accounting and existing tag
behavior. Only that composed production path is suitable for a wider external
`test1.test` runner.

## Non-claim

This slice does not satisfy `html_tokenizer_conformance`. It also does not
satisfy `tree_builder_conformance`, does not close the remaining Z7 tokenizer
states, and does not change Z7 from `planned`.
