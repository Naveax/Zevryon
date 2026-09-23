# Z4 bounded style-to-layout invalidation foundation

This slice introduces the first Z4 production primitive without claiming the
full incremental-layout gate.

## Input identity

The primitive compares two `CssSemanticStyleWindowV1` snapshots describing the
same absolute bounded document window. Their terminal IDs must belong to the
same `CssComputedStyleDagV1` identity space.

The caller never supplies a complete document style array. The two bounded
terminal arrays already carry their absolute document begin/end ordinals and the
total logical document node count.

## Output

Every node whose canonical style terminal changed is invalidated exactly once.
Adjacent changed nodes are coalesced into one absolute half-open range.

For example, changes at ordinals 101, 102, 104 and 105 become:

- [101, 103)
- [104, 106)

Unchanged nodes produce no range.

## Bounds and failure semantics

The scan is O(window nodes), not O(document nodes). Node count, output range
count and comparison work all have explicit hard maxima.

Terminal IDs are validated against the supplied canonical DAG before they are
accepted. Candidate output is published only after the complete bounded scan
succeeds, so invalid input, work exhaustion, range exhaustion and allocation
failure preserve the caller's prior published invalidation result.

## Boundary

This is the exact bounded style-diff primitive. It intentionally does not yet
expand changes through descendants, ancestors, formatting contexts or layout
dependency edges. Those propagation rules require their own Z4 authority before
the `incremental_invalidation` gate can be admitted.
