# Z2F-1 — Bounded Text Paint Command Stream

## Boundary

Z2F-1 consumes the certified Z2E-3 viewport projection and emits a compact,
backend-neutral text paint stream. It does not rasterize glyphs or allocate GPU
resources. The stream retains source indices and immutable style handles so a
later backend can acquire glyph images and submit draws without rebuilding text
layout.

The work is bounded by the projected viewport plus overscan. Total document
lines, clusters, and shaped glyphs outside that projection do not contribute to
output size.

## Inputs

The builder requires:

- `ViewportProjection` for visible/overscan lines, fragment rectangles, safe
  caret edges, and selection rectangles;
- `LineFragmentLayout` for the certified visual fragment topology;
- `MultiRunShapedText` for retained HarfBuzz glyph arrays, font face IDs,
  directions, and exact scales;
- `GlyphClusterMap` for logical-cluster to glyph-owner spans;
- immutable backend style IDs, either one per shaped segment or one default text
  style;
- selection and caret style IDs;
- one viewport-relative clip size;
- explicit command, glyph-batch, fill-rect, and referenced-glyph limits.

## Compact records

The retained output contracts are fixed:

- `TextPaintCommandRecord`: **16 bytes**;
- `TextPaintClipRect`: **32 bytes**;
- `TextPaintFillRect`: **48 bytes**;
- `TextPaintGlyphBatch`: **64 bytes**.

A command record names one payload and one clip. Payload arrays remain typed so
backends do not need a variable-size command encoding or per-command heap
allocation.

## Paint order

Commands are partitioned in an invariant order:

1. selection background rectangles;
2. text glyph batches;
3. the optional active caret rectangle.

This ordering prevents selection fills from covering glyphs and guarantees that
the caret remains above both selection and text. The builder never sorts by
style or font across these layers.

## Clip semantics

The stream publishes exactly one clip record. Its origin is viewport-relative
`(0, 0)` and its size is supplied by the caller. Overscan commands are retained
and classified, but the backend can clip them through the same viewport clip.
Z2F-1 does not build nested clips or clipping stacks.

## Zero-copy glyph-span proof

For each projected fragment, Z2F-1:

1. resolves every logical cluster through `GlyphClusterMap`;
2. skips repeated continuation records belonging to the same merged HarfBuzz
   owner group;
3. validates segment ownership and the fragment-local owner range;
4. proves that all unique owner spans form one contiguous interval in the
   retained shaped segment;
5. sums signed `x_advance` values over that interval;
6. verifies that the magnitude equals the projected fragment inline size and
   that the sign matches LTR or RTL direction.

The resulting glyph batch stores only `segment_index`, `first_glyph`, and
`glyph_count`. Glyph records and font bytes are never copied.

An X9-only or otherwise zero-glyph fragment is skipped only when its projected
inline size is also zero. Non-zero geometry without glyph evidence fails closed.

## RTL origin

LTR batches use the projected fragment inline start as their HarfBuzz pen
origin. RTL batches use the projected fragment inline end:

`viewport_inline_start + inline_size`

This preserves the signed negative advances retained by HarfBuzz and avoids
rewriting RTL glyph order or converting advances to unsigned geometry.

## Safe coalescing

Adjacent fragments may be coalesced only when all of these are true:

- both are LTR;
- both reference the same shaped segment;
- style ID, face ID, `x_scale`, and `y_scale` match;
- source line and viewport baseline match;
- glyph spans are contiguous;
- source fragment indices are contiguous;
- visual inline origins are contiguous.

RTL fragments are deliberately left separate because consecutive visual
fragments do not imply a safe increasing glyph-array span under RTL ordering.
Coalescing changes no glyph data; it only expands one retained span.

## Styles

Style IDs are immutable backend handles. Z2F-1 does not interpret colors,
blending modes, antialiasing policy, or font synthesis. A caller can provide one
style ID per shaped segment or one default text style. Selection and caret use
independent fill styles.

## Caret selection

The optional caret selector contains a source line, source fragment, and logical
boundary. It must resolve to exactly one safe `ViewportCaretEdge`. Zero or
multiple matches fail closed, preserving bidi split-caret affinity established
by Z2E-3.

## Allocation and failure model

Construction uses two deterministic passes:

1. validate all topology, resolve glyph spans, simulate coalescing, and count
   exact outputs;
2. reserve each PMR vector exactly once and publish the same sequence.

The stream owns only compact command metadata. Any validation error, explicit
limit violation, arithmetic overflow, or PMR allocation failure leaves every
output vector empty.

## Complexity

For the bounded projection:

- time: `O(projected lines + projected fragments + referenced glyph owners +
  selection rectangles + projected carets)`;
- retained memory: `O(commands + glyph batches + fill rectangles)`;
- document content outside the projection is not scanned.

## Explicit exclusions

Z2F-1 does not implement:

- glyph rasterization or ink-bound extraction;
- glyph atlas, raster tile, or GPU resource management;
- color-font paint graphs;
- transforms, zoom, or device-pixel conversion;
- clipping stacks beyond the single viewport clip;
- blending, compositing, or compositor surfaces;
- images, replaced elements, ruby, or vertical writing modes;
- DOM range ownership or accessibility projection;
- backend draw submission.

Those remain later Z2F stages.
