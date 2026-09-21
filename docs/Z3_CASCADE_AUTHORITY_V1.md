# Z3 configured author-origin cascade authority v1

This authority admits the configured Z3 `cascade_conformance` gate without
promoting Z3.

## Production path

Every authority decision runs through the production chain:

1. `parse_css_stylesheet_v1`;
2. `compile_css_compound_selector_v1`;
3. `match_css_compound_selector_v1`;
4. `cascade_css_author_rules_v1`.

The semantic node is a `div` with `id=hero` and `class=card`, so all four
frozen selector profiles match it.

## Exact precedence matrix

The authority uses four strictly increasing specificity profiles:

- `*` = (0,0,0);
- `div` = (0,0,1);
- `.card` = (0,1,0);
- `#hero` = (1,0,0).

For each first/second profile pair, both declarations independently take normal
or `!important` state. That yields exactly 64 winner decisions.

For every decision the authority compares the winning:

- value;
- important flag;
- specificity tuple;
- source-order index.

The frozen matrix contains exactly:

- 16 replacements caused by importance;
- 12 replacements caused by higher specificity;
- 8 replacements caused by later source order.

## Failure authority

A separate sequence freezes exact fail-closed behavior for:

- rule-count limit;
- declaration-count limit;
- unique-property limit;
- aggregate work-unit limit.

Each failure must return its exact error kind and preserve the previous
successful cascade result byte-for-record at the winner level.

## Frozen upstream provenance

The existing component oracle remains part of the same authority workflow and
pins three WPT cases at
`web-platform-tests/wpt@15df54d4459b78242d32ae36f9c094a96972bedc`:

- `specificity-001.xht`;
- `specificity-007.xht`;
- `cascade-005.xht`.

## Boundary

This is a configured author-origin cascade profile. It does not claim user or
user-agent origins, cascade layers, inline-style origin precedence, inheritance,
initial/unset/revert semantics, shorthand expansion, computed-value
normalization or selector syntax beyond the bounded compound-selector profile.

Those exclusions are explicit rather than quietly being counted as passing.
