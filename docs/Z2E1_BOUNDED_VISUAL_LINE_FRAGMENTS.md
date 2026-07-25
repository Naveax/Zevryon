# Z2E-1 — Bounded Visual Line Fragments

## Purpose

Z2E-1 converts Z2D-2 logical line selections into compact fragment slices ordered from visual inline start to inline end. It bridges the grapheme-cluster domain used by shaping and line selection with the X9-active scalar domain used by the certified UAX #9 visual-order stage.

## Inputs

The stage consumes immutable:

- grapheme boundaries;
- explicit bidi units and X9-active topology;
- final implicit bidi levels and paragraph level;
- multi-run HarfBuzz shaping output;
- logical glyph-cluster ownership;
- selected logical lines.

Every input must cover one identical cluster/codepoint domain. Inconsistent topology fails closed.

## Output contract

`VisualLineLayoutRecord` is exactly 24 bytes. Lines remain in logical block order and reference one contiguous fragment slice.

`InlineLayoutFragment` is exactly 32 bytes. Fragment slices are already in visual inline order and retain:

- inline offset and advance;
- shaped segment index;
- logical cluster range;
- L1-adjusted bidi level;
- compact direction/L1/X9 flags.

Glyphs are not copied. The fragment references the retained shaped segment and logical cluster range.

## Algorithm

1. Validate grapheme, bidi, shaping, cluster-map, and selected-line coverage.
2. Convert selected cluster spans into a contiguous partition of the X9-active scalar stream. Lines containing no active scalar are preserved but omitted from the temporary UAX #9 line-span array.
3. Run the existing certified `resolve_bidi_visual_order` implementation to obtain UAX #9 L1-adjusted scalar levels.
4. Compress adjusted levels to grapheme clusters. Opposite parities inside one grapheme are rejected; same-parity mixed levels retain the first active level, matching the shaping-run plan contract.
5. Recompute one signed-net HarfBuzz advance per owner cluster and build a checked prefix sum.
6. Split logical fragments only at shaped-segment or adjusted-level boundaries. A split inside one merged HarfBuzz glyph group is rejected.
7. Apply UAX #9 L2 to each line's fragment slice. L3 remains inside grapheme-atomic shaped runs and therefore does not create a fragment boundary.
8. Publish exact-size line and fragment vectors together only after every check succeeds.

## Failure atomicity

The caller-visible output is released before processing and remains empty after every failure. Temporary scalar levels, visual indices, cluster metadata, advance prefix data, and final records use the caller-provided PMR resource.

## Certification corpus

The fixed benchmark uses:

- 65,536 grapheme clusters and active bidi units;
- 1,024 selected lines;
- 4,096 shaped segments and output fragments;
- 32,768 merged glyph owner groups;
- four fragments per line with logical levels `[0, 1, 2, 1]`;
- visual segment order `[0, 3, 2, 1]` after L2;
- 2,048 units of inline advance per line.

Expected retained output is 155,648 bytes. The exact PMR peak gate is 1,277,960 bytes, including temporary active line spans, UAX #9 level/order vectors, cluster metadata, checked advance prefixes, and retained output.

## Explicit exclusions

Z2E-1 does not implement font metrics, baseline calculation, block progression, CSS whitespace collapsing, justification, hyphen insertion, vertical writing modes, painting, hit testing, accessibility projection, clipping, or rasterization.
