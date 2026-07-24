# Z2B-10 Immutable Catalog Font Binding and I/O-Free Shaping

## Purpose

Z2B-9 resolves one immutable catalog face to a bounded, content-addressed
`VerifiedFontResource`. The existing HarfBuzz path accepts that immutable
resource. Z2B-10 joins the systems without repeating platform parsing, file
reads, content hashing, or cache lookup for every shaped segment.

The contract has two explicit phases:

```text
Cold bind, once per selected catalog face
FontCatalogGeneration + FontFaceId
  -> platform identity locator
  -> bounded stable file loader
  -> content-addressed verified resource cache
  -> CatalogFontFaceBinding

Hot shape, repeatedly
CatalogFontFaceBinding + segmented text
  -> retained-resource HarfBuzz shaping
  -> ShapedGlyphRun
```

## Immutable binding

`bind_catalog_font_face` performs the only catalog/resource-resolution work.
It publishes a value-type `CatalogFontFaceBinding` containing retained shared
ownership of:

- the immutable `FontCatalogGeneration`;
- the selected `VerifiedFontResource`;
- the generation-local `FontFaceId`.

A failed initial bind or failed rebind clears the output binding. Resolver,
locator, file-loader, cache, parser, and integrity diagnostics remain on the
existing `CatalogFontResourceError` surface.

The binding remains valid when:

- the caller releases its generation handle;
- the generation store publishes a newer snapshot;
- the verified-resource cache evicts or clears the resident entry.

## Hot shaping

`shape_bound_catalog_harfbuzz_segment` accepts no generation, path, cache,
staging limit, raw font bytes, or caller-defined sfnt face index. It locally
retains both shared handles from the binding and passes only the immutable
resource into the existing verified HarfBuzz path.

The hot path performs:

- no platform-identity parsing;
- no UTF-8 path conversion;
- no filesystem metadata or payload read;
- no content hashing;
- no verified-resource cache lookup or mutation;
- no SFNT/TTC reparsing or integrity rescan.

Prior glyph output is released before validation. Invalid bindings, malformed
segment ranges, backend failures, and glyph-budget failures publish no glyphs.

## Required certification

The focused real-font test uses DejaVu Sans and proves:

- one cold bind publishes exact generation, face, resource, and cache-build evidence;
- external generation ownership can be released after binding;
- the verified-resource cache can be cleared after binding;
- two later hot shapes succeed from the immutable binding;
- the complete cache snapshot is byte-for-byte unchanged by hot shaping;
- repeated hot output is byte-exact;
- direct retained-resource shaping and bound shaping are byte-exact;
- a default/invalid binding clears prior glyph output;
- invalid shaping ranges preserve the nested `HarfBuzzShapingError`;
- a one-byte glyph budget publishes no output;
- invalid face and missing-file binds preserve resolver/file diagnostics;
- failed rebinds clear a previously valid binding;
- strict GCC and Linux ASan/UBSan pass.

## Explicit boundary

This slice does not choose fallback faces, shape multiple catalog runs in one
call, cache HarfBuzz blobs/faces/fonts/shape plans, memoize path metadata,
perform asynchronous I/O, derive language/features/variations, build caret
maps, break lines, rasterize glyphs, or paint text.
