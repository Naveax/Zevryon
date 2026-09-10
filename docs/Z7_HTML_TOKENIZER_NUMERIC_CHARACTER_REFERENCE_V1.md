# Z7 bounded numeric character-reference v1

## Scope

This slice adds a shared bounded character-reference component for the production tokenizer and integrates it into the admitted Data tag tokenizer in both Data and attribute-value return contexts.

The component begins at an input `&` and returns replacement UTF-8 plus the next input offset. It does not fabricate tokenizer tokens. The caller appends the replacement to its current Character or attribute-value buffer under the caller's existing token-byte bound.

## Admitted behavior

V1 admits:

- literal ampersand fallback when `&` is followed by EOF or a byte that cannot begin a named/numeric reference;
- decimal numeric references;
- hexadecimal numeric references with `x` or `X`;
- `absence-of-digits-in-numeric-character-reference` recovery;
- `missing-semicolon-after-character-reference` recovery with correct reconsume offset;
- U+0000 replacement with U+FFFD;
- out-of-Unicode-range replacement with U+FFFD;
- surrogate replacement with U+FFFD;
- noncharacter diagnostics while preserving the referenced scalar;
- control-character diagnostics and the WHATWG C1 replacement table;
- UTF-8 emission for decoded scalars;
- use from both Data and attribute-value contexts.

Complete named character references were explicitly outside this numeric slice at admission time. The later named-reference slice extends the same shared component using the separately pinned complete table; this document retains the numeric slice's independent scope.

## Input/output authority boundary

The surrounding v1 tokenizer still accepts only ASCII raw input for its current preprocessing/location authority. That does not mean decoded output is ASCII-only. Numeric references may produce multi-byte UTF-8 output, for example `&#x80;` maps through the C1 table to U+20AC EURO SIGN.

Decoded replacement bytes are appended through a separate bounded byte path and count toward the existing Character-token or attribute-value byte budget.

## Accounting and failure behavior

Parse errors produced by the shared reference component are emitted through the same tokenizer sink and are merged into Data-tag parse-error accounting on both successful and fail-closed returns.

The Data tokenizer is streaming and non-transactional. Previously published complete tokens remain published if a later unsupported named reference fails closed. Within the current Character or tag token, no token is published until that token satisfies its admitted validation and byte bounds.

## Focused regression authority

`html-tokenizer-numeric-character-reference-v1-tests` covers:

- literal `&`, `&&`, and `& ` fallback/reconsume behavior;
- digitless `&#` / `&#x` recovery and exact error positions;
- decimal and hexadecimal decoding;
- missing-semicolon recovery and next-offset semantics;
- U+0000, surrogate, out-of-range, noncharacter and C1-control numeric end-state rules;
- explicit named-reference fail-closed behavior;
- Data Character coalescing around references;
- quoted and unquoted attribute reference decoding;
- the pinned `test1.test` numeric attribute example;
- literal ampersand attribute fallback before whitespace and tag close;
- multi-byte UTF-8 replacement under the token-byte hard cap.

## Nonclaims

This slice does not implement or claim:

- the complete WHATWG named-character-reference table;
- ambiguous-ampersand/named-reference longest-match rules;
- named-reference attribute-context legacy-semicolon exceptions;
- full input-stream preprocessing or U+0000 raw-input replacement;
- general non-ASCII raw-input location authority;
- CDATA;
- full `tokenizer/test1.test` success;
- `html_tokenizer_conformance` gate closure;
- `tree_builder_conformance` gate closure;
- Z7 completion.

Z7 remains `planned`.
