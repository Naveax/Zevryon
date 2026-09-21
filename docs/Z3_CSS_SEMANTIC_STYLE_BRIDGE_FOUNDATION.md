# Z3 bounded semantic-window to style-DAG bridge foundation

This slice connects the already authoritative bounded Z8 semantic-node
projection to the Z3 selector, author-cascade and computed-style DAG surfaces.
It does not promote Z3 and does not admit a new gate.

## Production path

`compute_css_style_terminals_for_semantic_window_v1` consumes one
`ZenithSemanticNodeWindowResult`. It never opens the logical-node arena and it
never requests the full DOM. For each node in that bounded window it exposes
the node tag and original HTML attributes as `CssSelectorNodeV1` views, runs
the existing bounded author cascade, and interns the winning property/value set
into the canonical computed-style DAG.

The published result contains:

- the absolute document begin ordinal;
- the absolute document end ordinal;
- the total logical document node count;
- exactly one style-DAG terminal ID per bounded semantic node.

That shape is intentionally compatible with the bounded terminal candidate
input used by the offscreen materialization window.

## Shared bounds

The bridge revalidates the Z8 window before any cascade work:

- ordinal range and node count must agree exactly;
- each semantic node's authoritative attribute count must equal its payload;
- per-node and total attribute ceilings are explicit;
- per-node and total selector-semantic byte budgets are explicit;
- one work counter covers bridge preflight, cascade work and style-DAG work.

The cascade and style-DAG calls receive only their remaining share of the
bridge work budget. A nested layer therefore cannot quietly spend its own full
budget once the outer bounded operation is nearly exhausted.

Published terminal output is candidate-built and swapped only after every node
succeeds.

## Inline style boundary

The Z7/Z8 semantic node has a decoded `style=""` field while also preserving
the original attribute entry. This bridge does **not** ignore it.

A non-empty inline style fails in preflight, before any DAG mutation. The
current cascade authority models author stylesheet rules but does not yet model
the separate inline-style precedence level. Pretending an inline declaration
is merely a high-specificity stylesheet rule would produce incorrect
`!important` and origin ordering, so this slice fails closed instead.

Inline-origin declaration parsing and precedence is the next distinct CSS
integration slice.

## Cache-like DAG side effects

Simple input-shape failures, inline-style rejection and bridge preflight-budget
failure occur before the DAG is touched. Once cascade/style interning begins,
an error on a later node may leave canonical styles from earlier successful
nodes interned in the persistent DAG. Those nodes are deterministic,
content-addressed cache state and do not publish a partial terminal window.
The output contract remains atomic.

## Deliberate exclusions

This bridge does not claim CSS inheritance, computed-value normalization,
combinators or pseudo classes outside the current selector profile, stylesheet
discovery from HTML, mutation propagation, layout construction, or paint
construction. Those remain separate authority work rather than being smuggled
through an integration adapter.
