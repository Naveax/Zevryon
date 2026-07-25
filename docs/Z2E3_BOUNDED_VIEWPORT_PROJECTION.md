# Z2E-3 — Bounded Viewport Projection and Hit Testing

## Purpose

Z2E-3 projects certified Z2E-2 line boxes into one viewport-relative geometry window. Work and retained output are bounded by the caller's visible-line, fragment, caret and selection limits rather than by total document size.

## Inputs

The stage consumes immutable:

- Z2E-2 line boxes and per-fragment block metrics;
- Z2E-1 visual-order fragments;
- retained multi-run HarfBuzz glyphs;
- the certified logical glyph-cluster map;
- the certified safe-caret boundary map;
- inline and block viewport ranges plus overscan;
- an optional ordered logical selection boundary range;
- explicit maximum output counts for every record category.

All inputs must share one complete line, fragment and cluster domain. Inconsistent topology fails closed.

## Output contract

`ViewportLineRecord` is exactly 64 bytes and retains the source line index, viewport-relative block start and baseline, and slices into the projected fragment, caret and selection arrays.

`ViewportFragmentRect` and `ViewportSelectionRect` are exactly 48 bytes. `ViewportCaretEdge` is exactly 40 bytes.

Coordinates are signed viewport-relative layout units so leading overscan can remain negative. Sizes and the total document block extent remain unsigned 64-bit values.

## Algorithm

1. Validate line-box, visual-fragment, block-metric, shaped-segment, glyph-cluster and caret domains.
2. Binary-search the first and limiting source lines intersecting the block viewport plus overscan.
3. Cull visual fragments against the inline viewport plus overscan.
4. Reconstruct each projected fragment's glyph-owner advances from retained HarfBuzz positions using `abs(sum(x_advance))`.
5. Emit only certified safe caret boundaries. Boundaries inside merged groups or carrying unsafe evidence are never synthesized.
6. Traverse LTR groups in logical order and RTL groups in reverse logical order so caret records are emitted in visual inline order.
7. Preserve multiple visual caret locations for one logical bidi boundary. Equal-position affinity is deterministic: visual-start bias chooses the first record and visual-end bias chooses the last.
8. Intersect the optional logical selection with each projected visual fragment and emit one rectangle for every non-empty visual piece.
9. Sort each bounded line's caret slice by visual inline position and publish exact-size PMR vectors only after the count and emission passes agree.

## Hit testing

`hit_test_viewport_projection` performs no allocation. It selects the nearest projected line with a safe caret, then binary-searches that line's visual caret slice. Outside coordinates clamp to the nearest retained caret and report inline/block clamp flags.

The returned boundary is always a logical boundary already certified safe by `CaretBoundaryMap`.

## Failure atomicity

Caller-visible output is released before processing and remains empty after every failure. Output-count limits are checked before allocation. PMR allocation rejection publishes `OutputBudgetExceeded` without partial records.

## Certification corpus

The fixed benchmark retains a 16,384-line / 65,536-cluster document but projects only:

- 80 block-window lines, including eight lines of overscan on each side;
- 320 visual fragment rectangles;
- 640 safe visual caret records;
- 64 selection rectangles;
- 256 allocation-free hit-test queries per measured iteration.

Expected retained and peak projection memory is 49,152 bytes. The deterministic checksum is `1409705956279003952`.

## Explicit exclusions

Z2E-3 does not implement glyph ink bounds, CSS transforms, zoom/device-pixel conversion, replaced elements, ruby, vertical writing modes, DOM range ownership, accessibility projection, paint commands, clipping stacks, rasterization or compositor surfaces.
