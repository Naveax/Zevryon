# Z3 bounded offscreen style materialization foundation

This slice adds the first bounded style materialization batch primitive for Z3.
It does not promote Z3 and does not admit the full
`offscreen_materialization_bound` gate.

## Request model

`materialize_css_style_nodes_v1` accepts an explicit list of terminal computed
style DAG node identities. It does not decide which DOM nodes are visible,
offscreen or worth pre-materializing. That policy remains above this primitive.

Each unique terminal style is flattened at most once in a batch. Repeated
requests map through `request_style_indices` to the same materialized style
record. Root node zero materializes as an empty style.

This gives a bounded mechanism that a future viewport/offscreen policy can call
without forcing the entire style DAG, document or offscreen population to
materialize.

## Ownership

The output batch owns copied property/value text in caller-provided PMR
storage. The materialized result therefore survives replacement or release of
the source style DAG.

Output publication is atomic: construction happens in a candidate batch and
replaces the caller's previous output only after all requested styles
materialize successfully.

## Bounds

Hard configurable ceilings cover:

- request count;
- unique terminal styles;
- total materialized properties;
- retained output text bytes;
- properties per requested style;
- total work units.

Only requested root-to-terminal DAG paths are traversed. Unrelated DAG nodes
are not scanned. Chain topology, depth, text slices and canonical property
ordering are validated as they are visited.

Duplicate lookup, chain traversal, canonical-order checks and text-copy bytes
consume the shared work-unit budget. Retained output uses the caller PMR
resource; focused tests charge it to `ResourceClass::ComputedStyle`.

## Boundary

This foundation does not implement viewport classification, offscreen distance,
lookahead, DOM-to-style discovery, cross-batch caches, eviction, layout or
paint materialization. Those policies are required before the full
`offscreen_materialization_bound` gate can be admitted.
