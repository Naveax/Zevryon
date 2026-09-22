# Z3 offscreen materialization authority v1

This authority admits the Z3 `offscreen_materialization_bound` gate without
promoting Z3.

The authority combines the bounded materializer, a bounded document-order
candidate-window policy and both focused foundation oracles. The admitted claim
is deliberately about bounded offscreen style materialization, not layout or
paint.

## Exact million-node denominator without O(document) terminal RAM

The authority constructs 64 distinct computed styles in the production
persistent style DAG. The logical document contains 1,000,000 nodes, but the
window API receives that size only as a scalar `document_node_count`.

Only the exact visible-plus-lookahead candidate is resident:

- 1,000,000 logical document nodes;
- 1,536 candidate terminal IDs;
- 6,144 bytes of candidate terminal storage;
- 1,024 visible nodes;
- 256 before-lookahead nodes;
- 256 after-lookahead nodes;
- 1,536 selected requests;
- 1,536 selection work units;
- 64 unique materialized styles;
- 1,472 duplicate selected requests;
- 128 materialized properties;
- 492 materialized output text bytes.

The production window function verifies the candidate's absolute document
range and takes one bounded subspan. It neither scans nor requires a resident
terminal-style array for the other 998,464 logical nodes.

This matters for Zevryon's actual scale target. A complete 67,108,864-entry
`uint32_t` terminal array would consume 256 MiB before allocator/container
overhead, which is incompatible with the browser's low-memory contract. The
bounded candidate API removes that hidden O(document) prerequisite.

## Z8 compatibility

Z8's production semantic surface already reads bounded ordinal windows from the
disk-backed logical-node arena rather than projecting the full DOM into RAM.
The revised Z3 window contract matches that shape: an upstream bridge can derive
computed-style terminal IDs only for the bounded projected candidate and pass
those IDs with their absolute document base ordinal.

The authority does not claim that automatic DOM-to-terminal discovery bridge
yet. It certifies that the Z3 materialization surface no longer forces that
bridge to allocate a document-sized terminal array.

## Style-DAG denominator

The 64 authority styles share one `a:1` prefix. Before materialization, the
production DAG must retain exactly 66 nodes and 366 text bytes.

Each unique materialized style contains two properties. The candidate window is
24 complete repetitions of the 64-style set, so request mapping is checked for
all 1,536 selected document positions.

## Foundation requirements

The admitted authority also runs the bounded materializer foundation and
materialization-window foundation. Those cover atomic output replacement,
requested-path DAG validation, request/unique/property/text/work limits,
document-edge clipping, bounded candidate coverage, wrapper failure mapping and
the real `ResourceClass::ComputedStyle` hard-limit rejection path.

## Nonclaims

This authority does not claim pixel viewport geometry, scroll velocity,
automatic DOM-to-terminal discovery, layout object creation, paint object
creation, computed-value normalization or cross-window cache behavior. It
establishes that offscreen style materialization can be selected and executed
with a working set bounded by the configured candidate window rather than by
logical document size.
