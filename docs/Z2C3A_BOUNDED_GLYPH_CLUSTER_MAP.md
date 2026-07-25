# Z2C-3A — Bounded glyph-cluster map

## Purpose

Z2C-2 retains one exact `ShapedGlyphRun` per logical shaping run. Z2C-3A adds
an O(1) logical grapheme-cluster lookup without flattening or copying glyphs.
The map is the ownership and topology boundary required before caret positions,
selection geometry, line breaking, hit testing, and accessibility projection.

## Record contract

`GlyphClusterRecord` is exactly 16 bytes:

- `segment_index`: owner segment in `MultiRunShapedText`;
- `owner_cluster`: logical grapheme cluster that owns the HarfBuzz glyph group;
- `first_glyph`: first glyph offset inside that segment;
- `glyph_count`: contiguous glyph count in the owner group.

There is exactly one record for every logical grapheme cluster. When HarfBuzz
merges multiple grapheme clusters into one ligature or shaping cluster,
continuation clusters reference the same owner cluster and glyph group. Glyph
storage remains inside the original segment and is never duplicated.

## HarfBuzz boundary

The existing shaping backend sets
`HB_BUFFER_CLUSTER_LEVEL_MONOTONE_GRAPHEMES`. Z2C-3A therefore requires:

- non-decreasing cluster values for LTR segment glyph arrays;
- non-increasing cluster values for RTL segment glyph arrays;
- glyph cluster values inside the segment's logical cluster range;
- the lowest logical owner cluster equal to the segment start;
- contiguous segment ranges covering the complete logical cluster domain.

LTR owner groups are scanned forward. RTL owner groups are scanned backward so
both directions publish logical spans in ascending cluster order without a
second owner-group vector.

## Memory and failure model

The output performs one exact PMR resize to `cluster_count` records under the
appended `GlyphClusterMap` Resource Ledger class. The implementation allocates
no second dense cluster table and no flattened glyph array.

Malformed segment topology, out-of-range glyph clusters, non-monotone output,
unsupported vertical directions, compact-index overflow, and hard-budget
rejection all fail closed. A failed replacement releases any previously
published map.

## Initial certification

The first correctness fixture contains:

- one LTR segment over clusters 0–3;
- one RTL segment over clusters 4–7;
- an LTR ligature owner spanning clusters 0–1;
- a two-glyph LTR owner group at cluster 2;
- a two-glyph RTL owner group spanning clusters 4–5;
- exact glyph offsets inside both segment-local storage orders.

It requires exact eight-record output, six owner clusters, two continuation
clusters, correct LTR/RTL offsets, non-monotone rejection with exact location,
segment-gap rejection, stale-output clearing, one-byte hard-cap rejection, and
clean Resource Ledger accounting.

## Explicit boundary

Z2C-3A does not derive caret positions inside ligatures, split advances,
normalize visual run order, build line-break opportunities, create line
fragments, hit-test pixels, rasterize, or paint. Those remain Z2C-3B and later
Z2D–Z2F partitions.
