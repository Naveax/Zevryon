# Z3 inline author-cascade merge foundation

This slice adds the precedence layer needed before the semantic-window bridge
can safely consume a non-empty HTML style attribute.

It does not parse raw inline text. Instead it accepts an already parsed
CssDeclarationV1 span backed by a CssStylesheetV1, then merges those declarations
with an already resolved author stylesheet cascade.

## Current author-origin profile

Within the currently supported author profile:

1. important beats non-important;
2. when importance is equal, the inline declaration wins over an author
   stylesheet winner;
3. duplicate inline declarations resolve by importance and then later source
   order.

This gives the required cases without inventing a fake selector specificity:

- author important beats inline normal;
- inline important beats author normal;
- inline normal beats author normal;
- inline important beats author important at equal importance;
- a later normal inline duplicate cannot replace an earlier important inline
  declaration.

Normal property names arrive canonicalized by the existing parser. Custom
property names remain case-sensitive, so --X and --x are distinct.

## Style-DAG backing output

The style DAG requires one text store that owns every winning property/value
slice. CssInlineMergedCascadeV1 therefore copies the final winner set into a
bounded private CssStylesheetV1 text buffer and emits a matching
CssCascadeResultV1.

The winner provenance fields in this merged result are deliberately not
authority. Cascade ordering has already happened. They are placeholders because
intern_css_cascade_style_v1 consumes only the final property/value set after
winner selection. Reusing this merged result as input to another cascade stage
would violate the contract.

## Bounds and failure

Author property count, inline declaration count, output property count, output
text bytes and comparison/copy work all have explicit hard ceilings. Input
slices are revalidated before use. Candidate output is published only after the
entire merge succeeds, so work/text/input failures preserve the previous merged
winner set.

## Deliberate boundary

Raw HTML style-text parsing is intentionally separate. That adapter must pin its
parser recovery behavior after the configured parser authority lands. This
slice also does not claim user/user-agent origins, layers, animations,
transitions, inheritance or CSS-wide computed-value semantics.
