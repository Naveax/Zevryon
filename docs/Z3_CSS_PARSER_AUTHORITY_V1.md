# Z3 CSS parser configured-profile authority v1

This authority admits the configured Z3 `css_parser_conformance` gate without
promoting Z3 as a whole.

## Production boundary

The authority executes `parse_css_stylesheet_v1` through a dedicated C++
production probe. The probe serializes parser output through an ASCII line
protocol whose text fields are hex encoded, so NUL, newline, replacement
characters and arbitrary byte input cannot corrupt the runner protocol. On
Windows the probe forces stdin into binary mode before reading; the CRT must not
translate CRLF before CSS input preprocessing measures and normalizes it.

The Python runner feeds every configured input to the probe and compares:

- qualified rule selector text;
- declaration property/value text and `!important` state;
- retained at-rule context, owner, prelude and raw block payload;
- selected preprocessing/recovery statistics;
- exact parser error kind for fail-closed cases.

## Exact denominator

`config/z3_css_parser_corpus_v1.json` contains exactly 33 cases:

- 8 input-preprocessing cases;
- 10 declaration-list cases;
- 5 at-rule cases;
- 4 declaration-recovery cases;
- 6 fail-closed structural/name-boundary cases.

Eighteen cases carry frozen WPT provenance. Fifteen are local production
boundary cases required to freeze behavior not represented by the admitted WPT
slices.

The authority passes only when:

- configured cases = 33;
- passed = 33;
- failed = 0;
- unsupported = 0.

There is no runtime allowlist, xfail bucket or denominator reduction.

## Frozen upstream identity

The runner hard-codes
`web-platform-tests/wpt@15df54d4459b78242d32ae36f9c094a96972bedc`
and exact blob identities for:

- `css/css-syntax/input-preprocessing.html`;
- `css/css-syntax/declarations-trim-whitespace.html`;
- `css/css-syntax/missing-semicolon.html`;
- `css/css-syntax/charset-is-not-a-rule.html`;
- `css/css-syntax/at-rule-in-declaration-list.html`.

Changing any of those identities requires changing the authority code, not just
editing a data file until CI turns green.

## Cross-platform requirement

Ubuntu 24.04 and Windows Server 2022 both build and run:

1. the production parser component oracle;
2. the at-rule/recovery component oracle;
3. the production parser probe;
4. the complete 33-case authority runner;
5. the frozen scope contract.

Both platforms must pass on the exact PR head.

## Boundary

This gate is a configured production parser-profile claim, not a claim of every
CSS Syntax or CSSOM behavior defined by the platform. It does not claim
property grammar validation, CSSOM UTF-16 surrogate semantics, selector grammar
conformance, conditional at-rule evaluation, or the complete upstream WPT
directory.

Those boundaries are explicit so the gate means one immutable tested profile
instead of an elastic marketing sentence.
