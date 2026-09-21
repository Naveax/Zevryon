# Z3 bounded CSS at-rule and declaration-recovery foundation

This slice extends the production `parse_css_stylesheet_v1` path with bounded
generic at-rule consumption and a narrow invalid-declaration recovery path. It
does not promote Z3 and does not admit the full `css_parser_conformance` gate.

## At-rules

The parser now recognizes generic at-rules at the stylesheet top level and
inside style-rule declaration lists. Retained records contain a canonical
ASCII-lowercased name, trimmed prelude, optional raw block payload, and the
context in which the at-rule appeared. Declaration-list records also retain
the owning style-rule index; top-level records use an explicit no-owner
sentinel.

Top-level `@charset` directives are consumed and dropped rather than exposed
as retained rules. This is frozen against the pinned WPT
`charset-is-not-a-rule.html` case.

For `at-rule-in-declaration-list.html`, the foundation admits only the two
style-rule cases: a block-form at-rule and a semicolon-form at-rule followed by
`color: green`. The `@page` and `@font-face` cases remain outside this
slice because their declaration semantics require at-rule-specific grammar,
not merely generic CSS Syntax consumption.

## Invalid declaration recovery

A declaration failure classified as `InvalidDeclaration` no longer
necessarily aborts the entire stylesheet. The parser consumes the bad
declaration remnant until a top-level semicolon or the owning rule close, while
respecting strings, comments, escapes and bounded nested blocks. Parsing then
continues with later declarations.

Structural failures such as unterminated strings/comments/blocks, nesting-limit
rejection, allocation failure and output-budget rejection remain fatal. A
successful recovery clears the transient error record. Fatal failure preserves
the caller's previous stylesheet atomically.

## Bounds

At-rule count has a configurable hard limit with an implementation maximum of
1,048,576. At-rule blocks and recovery remnants share the parser's existing
nesting-depth ceiling, whose implementation maximum remains 256. Retained
at-rule text is charged to the same bounded output text and caller PMR resource
as qualified rules and declarations.

## Boundary

This is still not full CSS Syntax conformance. Conditional-rule evaluation,
`@page`/`@font-face` grammar, escaped at-keyword normalization, nested-rule
recovery, bad-string recovery, property grammar and selector grammar remain
separate authority work. The full `css_parser_conformance` gate stays
unadmitted.
