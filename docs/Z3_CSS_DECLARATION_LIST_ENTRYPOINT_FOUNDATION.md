# Z3 standalone CSS declaration-list entrypoint foundation

This slice exposes a native bounded declaration-list parser entrypoint for
future HTML style-attribute integration.

It deliberately does not manufacture a fake universal selector and qualified
rule around the input. The public surface is
parse_css_declaration_list_v1.

## Reuse instead of a second parser

The existing Parser class already owns the bounded declaration scanner,
important handling, at-rule sidecar, comments, strings, nesting checks and
invalid-declaration recovery used inside qualified rules.

The standalone entrypoint reuses that exact machinery. After normal CSS input
preprocessing it appends one internal closing-brace sentinel and invokes a
dedicated declaration-list driver. That sentinel lets the existing block-aware
value scanner treat raw EOF as the end of the final declaration without copying
the grammar into a second parser.

The sentinel is internal:

- input_bytes describes only caller input;
- preprocessed_input_bytes describes only caller input after preprocessing;
- no synthetic CssStyleRuleV1 is emitted;
- any closing brace supplied by the caller closes before the final sentinel, so
  the driver detects trailing bytes and fails closed.

## Output shape

Successful output is a CssStylesheetV1 used as bounded backing storage:

- rules is empty;
- declarations contains the standalone declaration list;
- normal property names retain existing lowercase canonicalization;
- custom property spelling remains case-sensitive;
- declaration-list at-rules, when present in the current parser profile, use
  CssAtRuleContextV1::DeclarationList with kCssAtRuleNoOwnerV1.

That output can feed the separate inline author-cascade merge layer without
inventing selector specificity.

## Failure and recovery

Invalid declarations use the same bounded remnant recovery path as declarations
inside style rules. Structural failures such as unterminated comments/strings
remain fatal. Candidate output is published only after the entire standalone
list succeeds, so failure preserves previously published output.

## Boundary

This is parser plumbing, not the inline-style cascade itself. It does not claim
HTML style-attribute integration, CSSOM declaration APIs, property grammar,
computed values, inheritance, or the full CSS Syntax declaration-list WPT
surface.
