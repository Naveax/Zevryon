# Z2C-2 — Bounded multi-run HarfBuzz shaping

## Purpose

Z2C-1 produces a logical-order, grapheme-atomic intersection of Script, font
fallback, final bidi level, and direction. Z2C-2 executes that immutable plan
through the existing catalog binding, prepared-face cache, and HarfBuzz segment
boundary without flattening all glyphs into a second full-size buffer.

## Input contract

- one validated `ShapingRunPlan` covering the complete grapheme domain;
- immutable catalog bindings sorted strictly by `FontFaceId`;
- every binding from the same generation ID and semantic fingerprint;
- one bounded `PreparedHarfBuzzFaceCache`;
- the unchanged decoded-codepoint and grapheme streams used to build the plan;
- shared language, feature, variation, scale, and unsafe-concat policy.

Extra unused bindings are permitted. Missing bindings, mixed generations,
invalid run descriptors, or explicit `Missing` fallback runs are rejected before
any glyph output is published.

## Output and memory model

`MultiRunShapedText` owns one outer PMR vector with exactly one
`MultiRunShapedSegment` for every logical shaping run. The outer vector reserves
the exact plan run count once under the appended `MultiRunShapeMetadata` ledger
class.

Each segment owns the existing `ShapedGlyphRun` value and therefore preserves
the existing single-run exact glyph allocation under `GlyphRun`. Glyphs are not
copied into a flattened paragraph vector, and the executor does not run
HarfBuzz twice merely to count output.

Referenced catalog faces are counted with a temporary PMR bitmap using one bit
per supplied binding. Preflight therefore remains `O(B + R log B)` instead of
performing a binding-by-run Cartesian scan; the bitmap is released before the
segment table is reserved, so its current bytes do not overlap retained output.

On any failure, every already-shaped segment is destroyed and both metadata and
glyph resources return to their pre-call ownership state. Cache preparation may
remain as a valid bounded side effect because prepared faces are immutable and
independently accounted.

## Execution semantics

For every logical run, Z2C-2:

1. resolves the exact binding by sorted `FontFaceId` lookup;
2. acquires or prepares the immutable HarfBuzz face through the bounded
   single-flight cache;
3. forwards global cluster indices, Script, direction, language, features,
   variations, and scale to the existing segment shaper;
4. marks beginning/end-of-text only at the real text boundaries;
5. retains the exact segment output and aggregates checked statistics.

No platform locator, font-file read, content hash, SFNT verification,
`hb_blob_create`, or `hb_face_create` occurs on a resident prepared-face hit.

## Fail-closed boundaries

The executor publishes no partial text for:

- malformed plan ranges or direction/level disagreement;
- an explicit missing-font run;
- an absent face binding;
- unsorted, invalid, or mixed-generation bindings;
- metadata hard-budget rejection;
- any nested cache or HarfBuzz shaping failure;
- aggregate counter or advance overflow.

## Certification surface

The real-font correctness certification uses one Latin LTR run and one Arabic
RTL run with two catalog face IDs backed by the same verified DejaVu Sans bytes.
It requires:

- exact two-segment output and global cluster continuity;
- byte-exact equality with two direct cache-backed segment calls;
- resident repeat determinism;
- shared-cache multi-thread single-flight behavior;
- exact missing-font, missing-binding, and mixed-generation errors;
- one-byte metadata rejection before shaping;
- failure on the second run after the first run has allocated glyphs, followed
  by complete metadata and glyph rollback;
- repeated non-adjacent face IDs with an exact distinct-face count;
- strict warnings-as-errors and Linux ASan/UBSan.

The permanent 64 KiB performance certification uses:

- 65,536 logical UTF-8 bytes;
- 49,152 code points and singleton grapheme clusters;
- 1,024 alternating Latin LTR and Arabic RTL shaping runs;
- two resident prepared catalog faces;
- identical PMR metadata and glyph resources for both compared paths;
- a manual loop over the existing cache-backed segment API as the baseline;
- the production multi-run executor as the measured path;
- exact equality of input dimensions, segment count, glyph records, glyph bytes,
  metadata bytes, advances, and output checksum.

### Calibration boundary

The first benchmark attempt was intentionally treated as development
calibration rather than final certification. It completed two alternating
distributions before its workflow was cancelled by a newer branch head. Both
paths produced exactly 49,152 glyphs, 1,376,256 glyph bytes, 81,920 metadata
bytes, equal advances, and the same output checksum. Observed P50 values were
`44.09–44.42 ms`, and executor/manual P50 ratios were approximately
`0.9995–1.0000`.

The speculative pre-calibration absolute gates of 25/30/45 ms were therefore
invalid for a workload containing 1,024 independent HarfBuzz calls. They were
never accepted as a passing certificate. The final envelope below was frozen
after calibration and must pass on a fresh exact head using five new independent
distributions.

### Frozen final gates

Five fresh in-process alternating distributions enforce:

- every executor/manual P50 ratio `<= 1.10`;
- median executor/manual P50 ratio `<= 1.03`;
- median executor P50 `<= 50 ms`;
- median executor P95 `<= 50 ms`;
- median executor P99 `<= 55 ms`;
- worst executor maximum `<= 65 ms`;
- retained metadata `<= 512 KiB`;
- retained glyph output `<= 4 MiB`;
- zero accounting errors and no hard-limit violation.

No final gate may be changed using measurements from the five-distribution
certification run.

## Explicit boundary

Z2C-2 does not choose fallback faces, provide a last-resort missing-glyph face,
reorder runs visually, flatten glyph storage, build glyph-to-cluster or caret
maps, break lines, create layout fragments, rasterize, or paint. Those remain
Z2C-3 and later Z2D–Z2F partitions.
