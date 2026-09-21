# Z3 bounded offscreen style selection foundation

This slice adds a deterministic bounded lookahead policy that feeds the
existing style materializer. It does not promote Z3 and does not admit the full
`offscreen_materialization_bound` gate.

## Input contract

The caller provides one candidate record per style-bearing node or other
upstream unit:

- terminal computed-style DAG node identity;
- whether the candidate intersects the current viewport;
- nonnegative CSS-pixel distance from the viewport for offscreen candidates.

The layout engine remains responsible for geometry. This primitive owns only
the bounded selection policy after those metrics exist.

## Selection policy

`select_css_offscreen_style_nodes_v1` applies these rules:

1. candidates intersecting the viewport are excluded from offscreen
   pre-materialization;
2. offscreen candidates beyond `lookahead_css_px` are excluded;
3. repeated terminal style IDs collapse to their nearest offscreen occurrence;
4. eligible unique styles are ordered by distance and then terminal node ID;
5. only the first `maximum_selected_styles` are published.

Zero lookahead is valid and selects only zero-distance offscreen candidates.

The output vector is directly consumable by
`materialize_css_style_nodes_v1`.

## Bounds and failure model

Candidate count, selected-style cap, lookahead distance and total work all have
fixed implementation maxima. Candidate scanning, duplicate lookup and
deterministic ordering consume the same explicit work budget.

Construction happens in candidate PMR storage and replaces the caller's prior
selection only after success. Candidate-limit, work-budget and allocation
failure therefore preserve prior output.

Focused tests also exercise real `ResourceClass::ComputedStyle` ledger
rejection.

## Boundary

This slice does not compute viewport geometry, discover DOM nodes or terminal
styles, predict scroll velocity, adapt lookahead distance, cache selections
across frames or implement eviction/aging.

Those upstream and lifecycle pieces remain separate work before the full
`offscreen_materialization_bound` gate can be admitted.
