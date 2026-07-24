# Z2B-10 Catalog-Backed HarfBuzz Shaping

## Purpose

Z2B-9 resolves one immutable catalog face to a bounded, content-addressed
`VerifiedFontResource`. The existing HarfBuzz path accepts that immutable
resource, but callers still had to join the two stages manually. Z2B-10 adds
one synchronous, failure-atomic bridge:

```text
FontCatalogGeneration + FontFaceId
  -> platform identity locator
  -> bounded stable file loader
  -> content-addressed verified resource cache
  -> retained-resource HarfBuzz shaping
  -> ShapedGlyphRun
```

## Contract

`CatalogHarfBuzzShapingRequest` owns no raw font bytes and accepts no caller
supplied sfnt face index. The resolver determines the selected face from the
retained generation and the platform identity. The verified resource remains
owned through the complete HarfBuzz call.

The bridge:

- retains the immutable catalog generation through resolution;
- resolves the requested face through Z2B-9;
- reuses the bounded verified-resource cache;
- passes only the immutable resource handle to HarfBuzz;
- preserves resolver and shaping errors as separate nested surfaces;
- releases prior glyph output before any work;
- publishes glyphs only after both stages succeed;
- reports whether resource resolution and shaping completed independently.

## Required certification

The focused test uses a real DejaVu Sans resource and proves:

- catalog-backed shaping succeeds through the retained-resource path;
- the second identical call is a verified-resource cache hit;
- repeated output is byte-exact;
- direct retained-resource shaping and catalog-backed shaping are byte-exact;
- invalid face IDs preserve `CatalogFontResourceError`;
- missing files preserve the nested `FontFileLoadError`;
- invalid shaping ranges preserve `HarfBuzzShapingError`;
- a one-byte glyph budget publishes no output;
- all glyph-output ledgers remain clean;
- strict GCC and Linux ASan/UBSan pass.

## Explicit boundary

This slice does not choose fallback faces, shape multiple font runs in one
call, cache HarfBuzz blobs/faces/fonts/shape plans, memoize path metadata,
perform asynchronous I/O, derive OpenType language/features/variations,
build caret maps, break lines, rasterize glyphs, or paint text.
