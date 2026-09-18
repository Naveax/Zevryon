# Z3 CSS declaration-list WPT authority

This authority freezes parser behavior that the merged production CSS parser
already implements. It adds no new production parsing code and does **not**
promote Z3 or admit the complete `css_parser_conformance` gate.

## Frozen WPT reference

The authority is pinned to
`web-platform-tests/wpt@15df54d4459b78242d32ae36f9c094a96972bedc`.

It covers:

- `css/css-syntax/declarations-trim-whitespace.html`
  (`a7d69d149e7bbf854efc2ef19af132af372ff521`), nine declaration variants.
- `css/css-syntax/missing-semicolon.html`
  (`d8e70e631591c05d2025d1800b1201ce80550fed`).
- its stylesheet fixture `css/css-syntax/support/missing-semicolon.css`
  (`0d9a0bbda757b8b4197d6e060f08657d12cf9e93`).

## Certified behavior

The production oracle requires exact declaration ordering and retained values
for all nine whitespace/`!important` variants. It also requires the final
declaration in a block to survive without a trailing semicolon and confirms
that braces inside comments do not corrupt block boundaries.

This is deliberately a declaration-list parser authority. It does not claim
property grammar validation, computed-value semantics, selector matching,
cascade winner selection, at-rule handling, nested recovery or full WPT
conformance. Z3 remains `planned`.
