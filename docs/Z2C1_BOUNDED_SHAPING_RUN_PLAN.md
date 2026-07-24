# Z2C-1 — Bounded Shaping-Run Plan

## Purpose

Z2A resolves Script and grapheme-atomic font fallback. Z1D resolves final bidi levels. Z2B shapes one already-segmented catalog run. The missing boundary is a deterministic, bounded plan that intersects those independent results without splitting a grapheme cluster.

Z2C-1 provides that boundary. It does not open font files, prepare HarfBuzz faces, shape glyphs, reorder lines, or break lines.

## Input contract

The planner consumes immutable, logical-order views:

- decoded Unicode code points;
- grapheme boundaries with a final sentinel;
- Script-run boundaries with a final sentinel;
- font-fallback boundaries with a final sentinel;
- explicit bidi units and X9-active topology;
- final bidi levels for every X9-active scalar;
- paragraph level 0 or 1.

Every boundary stream must cover the same grapheme cluster domain exactly. Active bidi units must be strictly ordered and must never reference an X9-removed unit.

## Output contract

`ShapingRunBoundary` is at most 16 bytes and contains:

- first grapheme cluster;
- selected catalog face, or the explicit missing-font sentinel;
- shaping Script;
- HarfBuzz direction;
- fallback source classification;
- representative final bidi level.

Non-empty input produces one boundary for every run plus one final sentinel. A run is split whenever Script, face/source, direction, or final embedding level changes.

## Grapheme atomicity

A complete grapheme cluster is never split. If active scalars inside one cluster have final levels of both parities, planning fails closed with `MixedClusterDirection`. X9-removed-only clusters inherit the preceding direction; leading removed-only clusters inherit the paragraph direction.

Same-parity level variation inside one grapheme is retained as one cluster and reported separately. The first active final level is the representative level.

## Memory and publication

The planner performs two passes:

1. validate, count exact output, and collect statistics without a dense temporary array;
2. reserve once and publish the exact boundary stream.

Output is released before validation and remains empty after every failure. The appended `ShapingRunPlan` Resource Ledger class accounts the persistent boundary vector without changing any existing enum ordinal.

## Initial certification surface

- intersection of Script, fallback, direction, and level boundaries;
- explicit missing-font runs;
- X9-removed-only direction inheritance;
- mixed-direction grapheme rejection;
- invalid topology and boundary rejection;
- one-byte output-budget rejection;
- strict GCC, AppleClang, MSVC, Linux ASan/UBSan, and macOS ASan/UBSan.

## Next milestones

1. **Z2C-2:** execute every non-missing planned run through automatic cache-backed catalog shaping and publish compact run/glyph descriptors atomically.
2. **Z2C-3:** build logical cluster-to-glyph and caret-safe boundary maps.
3. **Z2D-1:** Unicode line-break opportunity stream, independent from width measurement.
4. **Z2D-2:** width-constrained line selection over shaped advances and unsafe-to-break flags.
5. **Z2E:** bounded line fragments connected to the existing viewport/layout checkpoint path.
6. **Z2F:** retained text display-list commands and the first pixel-producing paint slice.

## Explicit boundary

Z2C-1 does not select fonts, load resources, invoke HarfBuzz, concatenate glyphs, build caret maps, apply visual line ordering, perform line breaking, create layout fragments, rasterize, or paint.
