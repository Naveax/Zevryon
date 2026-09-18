# Z3 CSS input preprocessing authority

This slice extends the production `parse_css_stylesheet_v1` path with bounded
input preprocessing before qualified-rule parsing. It does **not** promote Z3
and does not claim the full `css_parser_conformance` gate.

## Production behavior

The parser now normalizes raw UTF-8 byte input before syntax parsing:

- NUL bytes become U+FFFD.
- CRLF, CR and form-feed normalize to LF.
- malformed UTF-8 becomes deterministic U+FFFD replacement.
- a well-formed WTF-8 surrogate sequence is consumed as one invalid scalar and
  becomes one U+FFFD replacement.
- the temporary preprocessed buffer uses the same caller-provided PMR resource
  as retained CSS output, so it is charged to `ResourceClass::ComputedStyle`.

The stats surface reports raw bytes, preprocessed bytes and each replacement or
newline-normalization count independently.

## Frozen external reference

The authority references
`web-platform-tests/wpt@15df54d4459b78242d32ae36f9c094a96972bedc`,
`css/css-syntax/input-preprocessing.html`.

Exactly the five WPT NUL-to-U+FFFD cases are admitted into this byte-input
authority. The five JavaScript surrogate cases are deliberately excluded
because Zevryon's production entry point consumes UTF-8 bytes rather than
JavaScript UTF-16 code units. The production tests separately cover a
surrogate-encoded invalid UTF-8 sequence without pretending that this is the
same CSSOM API contract.

## Remaining Z3 work

At-rules, CSS error recovery, nested rules, selector matching, cascade,
specificity, dependency invalidation, bounded style-DAG sharing and offscreen
materialization remain outside this slice. Z3 therefore remains `planned`.
