# Z2B-12B Cache-Backed Catalog HarfBuzz Shaping

## Purpose

Z2B-12A stores immutable prepared HarfBuzz faces under bounded retention,
metadata, LRU, and single-flight rules. Z2B-12B joins that cache to the
catalog-facing shaping API so callers need one operation rather than a manual
`get_or_prepare` followed by a prepared shaping call.

```text
CatalogFontFaceBinding + segmented text
  -> PreparedHarfBuzzFaceCache::get_or_prepare
  -> prepared catalog shaping hot path
  -> ShapedGlyphRun
```

## Contract

`shape_cached_catalog_harfbuzz_segment` copies the binding before cache work,
acquires one immutable prepared face, retains it through the complete shaping
call, and publishes glyphs only after both stages succeed.

The error and statistics surfaces remain split:

- prepared-face cache admission/preparation errors;
- bound catalog shaping errors;
- cache acquisition completion;
- shaping completion.

On a resident cache hit the call performs no platform locator work, file I/O,
content hashing, SFNT parse/checksum verification, `hb_blob_create`, or
`hb_face_create`. It still creates request-local `hb_font_t` and `hb_buffer_t`
objects, preserving scale/variation isolation and concurrent shaping safety.

## Failure atomicity

Prior glyph output is released before validation. Invalid bindings fail before
cache mutation. Cache failures publish their complete nested cache error and no
glyphs. Shaping failures preserve both the bound-shaping and HarfBuzz error
layers. A one-byte glyph budget publishes no output even after a successful
cache hit.

## Certification

The real DejaVu Sans test proves:

- first call performs exactly one preparation and shapes through the prepared
  path;
- second call is a resident hit with byte-exact glyph output;
- automatic cache-backed and direct prepared paths are byte-exact;
- verified-resource cache counters remain unchanged;
- twelve concurrent shaping calls share exactly one preparation and publish
  identical glyphs;
- invalid binding, retention rejection, invalid cluster, and glyph-budget
  failures preserve the correct nested error layer;
- all glyph ledgers remain clean.

## Performance gate

The dedicated benchmark alternates three real catalog-facing paths in one
process:

1. plain binding, which rebuilds HarfBuzz blob/face per call;
2. direct prepared handle;
3. automatic cache-backed prepared hit.

All paths must publish identical generation, face, resource, glyphs, advances,
UPEM, and memory accounting. Before measurement, the following P50 gates are
fixed:

- every automatic/direct-prepared ratio `<= 1.35`;
- median automatic/direct-prepared ratio `<= 1.20`;
- every automatic/plain-binding ratio `<= 0.90`;
- median automatic/plain-binding ratio `<= 0.75`.

## Explicit boundary

This slice does not hide cache construction in a global singleton, perform
async preparation, persist cache entries, cache `hb_font_t`/`hb_buffer_t`,
cache shape plans, select fallback faces, shape multiple runs, build caret
maps, break lines, rasterize, or paint.
