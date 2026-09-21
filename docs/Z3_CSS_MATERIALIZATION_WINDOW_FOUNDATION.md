# Z3 bounded style materialization window foundation

This slice adds the first document-order selection policy above the bounded
style materializer. It advances `offscreen_materialization_bound`, but does
not admit the gate and does not promote Z3.

## Selection contract

`materialize_css_style_window_v1` receives a span of terminal computed-style
DAG identities in document order plus a half-open visible range
`[visible_begin, visible_end)`.

The function clips a configured number of document nodes before and after the
visible range, computes one contiguous selected range, and passes only that
subspan to `materialize_css_style_nodes_v1`.

There is no loop over the full document-terminal span in this production
surface. A 100,000-node oracle with one visible node and two-node lookahead on
each side selects exactly five requests and charges exactly five selection work
units. Humanity has occasionally discovered that not touching 99,995 unrelated
things is an optimization.

## Bounds

The wrapper has fixed implementation maxima for visible nodes, offscreen nodes
per side and selection work units. Its configured worst-case window must fit
the nested materializer's request capacity.

Visible-range overflow and selection-work exhaustion fail before nested
materialization. The nested materializer continues to enforce its own unique
style, property, text and work budgets.

## Mapping and failure

`stats.selected_document_begin` and `selected_document_end` identify the
exact document range sent to the materializer. Output request mapping index
`i` therefore corresponds to document index
`selected_document_begin + i`.

Wrapper failures do not touch prior output. Nested failures are surfaced as
`MaterializationFailure` with both the nested error kind and the exact
document index, while the nested candidate-output contract preserves prior
materialized output atomically.

## Boundary

This is document-order lookahead, not final viewport policy. Pixel geometry,
scroll velocity, non-contiguous distance ranking, automatic DOM-to-style
discovery, layout/paint construction and cross-window caching remain outside
this foundation. The full `offscreen_materialization_bound` gate stays
unadmitted.
