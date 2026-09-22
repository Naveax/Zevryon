# Z3 bounded style materialization window foundation

This slice adds a bounded document-order selection policy above the style
materializer. It advances `offscreen_materialization_bound`, but does not admit
that gate and does not promote Z3.

## Bounded input contract

`materialize_css_style_window_v1` no longer accepts a terminal-style array for
the complete document. The caller supplies:

- the total logical document node count as a scalar;
- one bounded contiguous candidate span of computed-style terminal IDs;
- the candidate span's absolute document begin ordinal;
- an absolute half-open visible range `[visible_begin, visible_end)`.

The candidate span may not exceed the configured worst-case
`visible + before + after` window, which itself must fit the nested
materializer request capacity. The function computes the clipped selected
range in constant work, verifies that the bounded candidate covers it, and
passes only that subspan to `materialize_css_style_nodes_v1`.

A 100,000-node logical-document oracle therefore supplies exactly five terminal
IDs for one visible node plus two-node lookahead on each side. No O(document)
terminal-ID allocation is required merely to select the style window. Humanity
has been spared one particularly avoidable vector.

## Bounds

The wrapper has fixed implementation maxima for visible nodes, offscreen nodes
per side and selection work units. Candidate input is additionally rejected if
it exceeds the configured worst-case window.

Visible-range overflow, malformed candidate coverage and selection-work
exhaustion fail before nested materialization. The nested materializer continues
to enforce unique-style, property, text, traversal and allocation budgets.

## Mapping and failure

`stats.candidate_document_begin/end` expose the exact bounded input range.
`stats.selected_document_begin/end` expose the exact absolute document range
sent to the materializer. Output request mapping index `i` therefore
corresponds to absolute document index `selected_document_begin + i`.

Wrapper failures do not touch prior output. Nested failures surface as
`MaterializationFailure` with both the nested error kind and the absolute
document index, while the nested candidate-output contract preserves prior
materialized output atomically.

## Z8 compatibility boundary

Z8 already exposes bounded disk-backed semantic node windows instead of a
resident complete DOM projection. This Z3 API now has the same bounded-window
shape: an upstream bridge can compute terminal styles for only the projected
candidate window and pass those IDs here. Automatic DOM-to-terminal discovery
is still separate work and is not claimed by this foundation.

## Boundary

This is document-order lookahead, not final pixel viewport policy. Pixel
geometry, scroll velocity, non-contiguous distance ranking, automatic
DOM-to-terminal-style discovery, layout/paint construction and cross-window
caching remain outside this foundation. The full
`offscreen_materialization_bound` gate stays unadmitted.
