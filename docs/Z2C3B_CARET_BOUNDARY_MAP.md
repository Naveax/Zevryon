# Z2C-3B Caret Boundary Map

Z2C-3B derives a compact logical caret-boundary index from the retained multi-run shaping output and the Z2C-3A glyph-cluster map.

## Retained memory

The output contains exactly one byte for each logical boundary, including the terminal boundary. A document with 65,536 clusters therefore retains 65,537 bytes. Glyph arrays, advances, and cluster records are not copied.

## Boundary classification

Each byte records whether the boundary is a text edge, shaping-run edge, glyph-group edge, merged-group interior, or adjacent to HarfBuzz break-restriction evidence. Text edges are valid caret positions. Merged-group interiors are not exposed as caret positions. Clean glyph-group edges remain available unless adjacent shaping evidence requires a conservative rejection.

## Validation

The builder validates cluster owners, segment order, segment transitions, glyph-span bounds, glyph ownership, and complete retained-glyph coverage. It clears previous output before validation and publishes the candidate map only after complete success.

## Complexity

Construction is O(clusters + glyphs + segments). Boundary queries are O(1) and allocate no memory.

## Certification workload

The focused workload contains 65,536 clusters, 2,048 alternating LTR and RTL segments, 40,960 glyphs, and 32,768 merged two-cluster groups. Expected retained output is exactly 65,537 bytes.

## Scope boundary

This layer does not calculate pixel coordinates, split glyph advances, implement OpenType ligature caret tables, perform line breaking, produce selection rectangles, hit-test pixels, rasterize, or paint.
