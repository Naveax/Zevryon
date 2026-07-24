# Z2C-2 — Bounded Multi-Run Catalog Shaping

## Purpose

Z2C-1 publishes a logical-order sequence of grapheme-atomic Script/font/direction/level runs. Z2B-12B shapes one catalog run through the bounded single-flight prepared-face cache. Z2C-2 joins those two completed contracts and publishes one atomic glyph arena for a complete materialized text window.

This stage remains below line breaking and visual line ordering. It does not select fallback faces, reorder runs, build carets, choose line widths, create layout fragments, rasterize, or paint.

## Planned request contract

The executor will consume:

- decoded code points and grapheme boundaries;
- a validated `ShapingRunPlan`;
- a strictly sorted, unique table of `FontFaceId -> CatalogFontFaceBinding` references;
- one bounded `PreparedHarfBuzzFaceCache`;
- language, OpenType feature, variation, and scale settings;
- caller-owned PMR resources and hard limits for persistent output and temporary shaping state.

A binding entry must be valid and its embedded face ID must equal the table key. A non-missing planned run without a matching binding fails closed. Missing-font planned runs require no binding and remain explicit in output.

## Planned output model

### Run descriptor

Each logical run descriptor will record:

- first and limit grapheme cluster;
- first glyph and glyph count in the shared glyph arena;
- selected `FontFaceId`, or the missing-font sentinel;
- Script, direction, fallback source, representative bidi level;
- explicit flags for missing-font and empty-glyph outcomes.

Descriptors remain in logical plan order. They are not line-level visual-order records.

### Glyph arena

All shaped glyphs will use the existing 28-byte `ShapedGlyph` contract and preserve logical cluster indices from the source grapheme domain. No run-local cluster rebasing is allowed.

The executor will publish:

- an exact descriptor vector;
- one contiguous glyph vector;
- aggregate advances, unsafe-to-break/concat counts, missing glyphs, cache hits/misses, and prepared-face statistics.

## Failure atomicity

Persistent output is released before validation and remains empty after every failure. Errors retain three levels:

1. multi-run request/plan/binding failure;
2. prepared-face cache acquisition failure;
3. nested HarfBuzz shaping failure.

A failed run never exposes descriptors or glyphs from earlier successful runs.

## Exact-allocation strategy

The first implementation will prefer bounded determinism over minimum CPU:

1. validate the complete plan and binding table;
2. first shaping pass: shape each non-missing run into one reusable scratch run, validate clusters, and count exact glyph output;
3. reserve exact persistent descriptor and glyph capacities once;
4. second shaping pass: reproduce each run and publish into temporary persistent vectors;
5. atomically swap both vectors into caller-visible output.

The prepared-face cache makes both passes resident and I/O-free after the first acquisition. This intentionally trades a second HarfBuzz call per run for exact persistent allocation and no duplicate retained glyph arena. A later optimization may replace the second pass with bounded chunk retention only if benchmark evidence justifies the extra memory.

## Binding lookup

The initial binding table will be sorted by `FontFaceId`; executor lookup will use binary search. Pointer identity, resource ID alone, or unordered-map allocation will not define correctness. Each binding already retains generation fingerprint and content identity through the existing catalog contract.

## Initial correctness matrix

- one LTR run;
- alternating LTR/RTL runs;
- multiple Scripts sharing one face;
- multiple faces within one Script;
- explicit missing-font run between shaped runs;
- empty-glyph but valid shaped run;
- beginning/end-of-text flag propagation;
- exact global cluster preservation;
- unsorted, duplicate, mismatched, invalid, and missing binding rejection;
- first-pass and second-pass nested shaping failure;
- descriptor budget and glyph budget rejection;
- output remains empty after every failure;
- repeated call replacement without stale descriptors or glyphs.

## Initial performance certification

The benchmark will use a realistic materialized window, not the 65,536-run worst case from the planner benchmark. It will alternate Latin, Arabic, and another complex Script over multiple catalog faces and include explicit missing-font ranges.

The first thresholds will be fixed before measurement for:

- total P50/P95/P99 and maximum;
- glyph and descriptor resident bytes;
- temporary scratch peak;
- prepared-face cache preparations and resident hits;
- exact output equivalence between direct per-run shaping and the aggregate executor.

Thresholds may only tighten after the first artifact.

## Subsequent milestones

1. **Z2C-2A:** request/output structures, binding-table validation, missing-run publication, and failure surfaces.
2. **Z2C-2B:** two-pass automatic cache-backed shaping executor with exact publication.
3. **Z2C-2C:** real-font multi-Script benchmark, sanitizer matrix, and fixed memory/latency gates.
4. **Z2C-3:** cluster-to-glyph, glyph-to-cluster, caret-safe boundary, and selection maps.
5. **Z2D:** Unicode break opportunities followed by width-constrained line selection.

## Explicit boundary

Z2C-2 will not perform font fallback selection, platform font discovery, file I/O on resident hits, visual run ordering, line breaking, justification, hyphenation, caret placement, accessibility projection, layout-fragment construction, rasterization, or painting.
