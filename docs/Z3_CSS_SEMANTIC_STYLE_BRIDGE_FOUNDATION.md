# Z3 bounded semantic-window to style-DAG bridge foundation

This slice connects the bounded Z8 semantic-node projection to the Z3 selector,
author cascade, inline author-style precedence and computed-style DAG surfaces.
It does not promote Z3 and does not independently admit a gate.

## Production path

`compute_css_style_terminals_for_semantic_window_v1` consumes one
`ZenithSemanticNodeWindowResult`. It never opens the logical-node arena and
never scans the complete DOM.

For each bounded node it:

1. exposes tag and retained HTML attributes as `CssSelectorNodeV1` views;
2. resolves matching author stylesheet rules with
   `cascade_css_author_rules_v1`;
3. when `node.style` is non-empty, parses it with the native standalone
   `parse_css_declaration_list_v1` entrypoint;
4. rejects declaration-list at-rules at the HTML style-attribute boundary;
5. merges author winners and parsed inline declarations through
   `merge_css_author_and_inline_cascade_v1`;
6. interns the final property/value winner set through
   `intern_css_cascade_style_v1`.

The result publishes one style-DAG terminal ID for each bounded semantic node
plus the absolute document begin/end ordinals and total logical node count.

## Semantic payload consistency

The strict HTML producer stores a decoded style semantic and retains the
original `style` attribute in the attribute vector. The bridge revalidates
that split representation. Because those are two resident payloads, the
semantic style bytes are charged separately from the retained attribute
name/value bytes rather than being counted only once. A non-empty semantic style requires exactly one
retained style attribute with the same decoded value. An empty retained style
attribute must likewise agree with the empty semantic style. Contradictory
payloads fail as `InvalidWindow` before cascade work.

## Inline precedence

Inline declarations are not faked as a high-specificity stylesheet selector.
The dedicated merge layer applies the current author-origin profile:

- important beats non-important;
- at equal importance inline declarations beat author stylesheet winners;
- duplicate inline declarations resolve by importance and then later source
  order;
- normal properties use parser lowercase canonicalization;
- custom property names remain case-sensitive.

This preserves the required cases including author-important vs inline-normal,
inline-important vs author-normal, inline-normal vs author-normal, and
inline-important vs author-important.

## Shared bounds

The bridge keeps explicit node, attribute, semantic-byte and work ceilings.
Nested cascade, inline merge and style-DAG operations receive only the remaining
shared work budget. Inline parsing is charged by bounded input bytes before the
parser is invoked, while the parser's own input/declaration/nesting/output caps
remain authoritative.

Published terminal output is candidate-built and swapped only after every node
succeeds.

## Failure semantics

Input-shape failures and style-payload disagreement occur before DAG mutation.
For a single node, inline parse, at-rule-policy and inline-merge failures occur
before that node is interned. Nested inline-merge limit failures retain the
specific merge error kind so callers can distinguish configuration ceilings
from parser or DAG failures.

As with the pre-existing bridge contract, once execution has interned styles
for earlier nodes, a failure on a later node may leave deterministic canonical
cache nodes in the persistent DAG. It never publishes a partial terminal
window. The output window remains atomic.

## Deliberate exclusions

This bridge does not claim CSS inheritance, computed-value normalization,
unsupported selector profiles, stylesheet discovery from HTML, style-attribute
at-rules, CSSOM style mutation APIs, mutation propagation, layout construction
or paint construction.
