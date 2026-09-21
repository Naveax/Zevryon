# Z3 bounded computed-style DAG foundation

This slice adds the first persistent computed-style sharing primitive for Z3.
It does not promote Z3 and does not admit the full `bounded_style_dag` gate.

## Identity and sharing

`intern_css_cascade_style_v1` consumes an already-computed cascade winner set.
The style identity is only the final winning property/value set. Cascade
provenance such as `!important`, selector specificity and source order has
already served its purpose during winner selection and is intentionally not
part of computed-style identity.

Winner properties are canonicalized by bytewise property order before
interning. Node zero is the empty style. Every later node represents its parent
style plus one property/value assignment. Equal canonical prefixes therefore
reuse the same node, and equal complete styles return the same terminal node
even when they came from different rule order, specificity or importance
history.

This is a persistent prefix DAG. A tree is a valid DAG; later inheritance and
delta-sharing work may introduce richer graph structure without changing the
terminal style identity contract.

## Bounds

Property count, retained node count, retained text bytes, per-style semantic
bytes and work units all have configurable ceilings with fixed implementation
maxima.

The shared work budget covers:

- validation of all retained DAG nodes before mutation;
- property-order canonicalization comparisons;
- every retained node visited during child lookup;
- child property/value lookup comparisons.

Temporary canonical-order storage and retained DAG storage use the caller PMR
resource. Focused tests charge that memory to
`ResourceClass::ComputedStyle`.

## Failure model

Duplicate winning properties, corrupt cascade slices, corrupt DAG topology or
text slices, representation overflow, budget exhaustion and allocation failure
all fail closed.

Interning records the old logical node/text sizes and restores them on failed
extension. Container capacity may remain reserved after a failed allocation or
extension, but no partially-created style node remains logically visible.

## Boundary

This foundation does not implement inheritance, initial/unset/revert
semantics, shorthand expansion, computed-value normalization, style
invalidation, DAG eviction/compaction, persistence or offscreen
materialization. Those remain separate Z3 slices.
