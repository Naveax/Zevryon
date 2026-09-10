# Z7 HTML tokenizer markup declarations v1

## Purpose

This component is the bounded production markup-declaration tokenizer used by the composed Data-state tokenizer. It emits directly through `HtmlTokenizerV1Sink` / `HtmlTokenizerV1Token`; no DOM or node-source output is used as a substitute for tokenizer events.

The external tokenizer authority is the complete pinned `html5lib/html5lib-tests` corpus at commit `224991ec10db04f056a89eed8b0bd8695fd2950e`, whose provenance v2 denominator is **14 fixture files / 6810 test objects / 7036 initial-state executions**.

## Admitted component surface

`consume_html_markup_declaration_v1()` consumes one declaration beginning at `<` followed by `!`. The admitted surface includes:

- case-insensitive `<!DOCTYPE` recognition;
- bounded ASCII DOCTYPE-name handling with ASCII uppercase normalization;
- missing-whitespace-before-name recovery for an otherwise representable ASCII name;
- arbitrary representable ASCII bytes in the DOCTYPE name rather than the historical `[A-Za-z0-9_:-]` subset;
- whitespace / `>` transitions after the name;
- case-insensitive `PUBLIC` and `SYSTEM` keyword recognition;
- double- and single-quoted public/system identifiers with explicit null-vs-empty presence flags;
- the missing-whitespace-after-keyword and missing-whitespace-between-public/system-identifier recoveries;
- missing/abrupt quoted identifier diagnostics;
- invalid sequences after a DOCTYPE name and unexpected characters after a system identifier through the bogus-DOCTYPE state;
- EOF-in-DOCTYPE handling with force-quirks;
- HTML correctness-to-`force_quirks` event mapping;
- ASCII control-character diagnostics on newly admitted DOCTYPE input;
- standard and bounded comment / bogus-comment handling already admitted by the component;
- explicit token-byte caps and sink/stat accounting.

DOCTYPE payload accounting covers the name plus public and system identifier payloads. No partially constructed token is published when an unsupported or bounded-resource boundary is hit.

## Deliberate fail-closed boundaries

This slice remains intentionally narrower than full WHATWG input preprocessing. It does not manufacture support for:

- raw NUL replacement/input-stream preprocessing;
- non-ASCII DOCTYPE-name or identifier authority where preprocessing/location semantics are not yet admitted;
- missing DOCTYPE-name tokens whose external wire representation currently requires nullable-name support;
- non-ASCII comment / bogus-comment preprocessing authority;
- CDATA and unrelated tokenizer-state gaps.

For unsupported non-ASCII DOCTYPE inputs the production boundary retains the pre-existing census failure class instead of silently moving an execution from one unsupported reason bucket to another. This matters because the full-corpus `no-regression` authority checks every fixture and every failure/unsupported reason independently, not just global totals.

## External regression authority

The full-corpus census snapshot is **5660 pass / 129 fail / 1247 unsupported / 7036 total** before this recovery slice. Production tokenizer changes are checked with the census `no-regression` policy:

- `passed` may only stay equal or increase;
- `failed` and `unsupported` may only stay equal or decrease;
- those monotonic rules also apply independently to every fixture;
- existing failure/unsupported reason buckets may not increase and new positive failure/unsupported buckets are rejected.

Therefore this state-machine expansion is only admissible if previously unsupported executions become exact passes without creating a hidden regression elsewhere. A separate authority-only promotion is required before any improved observed distribution becomes the next frozen exact snapshot.

## Focused verification

The focused markup-declaration tests retain the original pinned `test1.test` DOCTYPE/comment regressions and add representative cases for:

- no-whitespace DOCTYPE names;
- punctuation in names;
- bogus-after-name recovery;
- PUBLIC and SYSTEM identifiers;
- public+system combinations;
- missing whitespace around identifier transitions;
- missing and abrupt identifiers;
- unexpected data after a system identifier;
- ASCII control characters;
- bounded DOCTYPE payload rejection;
- retained non-ASCII and NUL fail-closed boundaries.

Exact one-based parse-error locations are asserted.

## Non-claim

This component does not by itself satisfy `html_tokenizer_conformance`. Full-corpus pass remains false while any failed or unsupported execution exists. It also does not satisfy `tree_builder_conformance`, `chunk_size_equivalence`, `parser_fuzzing`, `bounded_large_document_parse`, or Z7 completion.
