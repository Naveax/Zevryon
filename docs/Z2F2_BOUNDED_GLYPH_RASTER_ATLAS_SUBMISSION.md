# Z2F-2: Bounded Glyph Raster Working Set and Atlas Submission

## Scope

Z2F-2 consumes the certified Z2F-1 text paint command stream and retained
`MultiRunShapedText`. It extracts only the glyph raster keys referenced by the
bounded viewport command stream, resolves those keys against a bounded atlas
metadata cache, and publishes backend-neutral upload and draw submission
records.

This boundary does not rasterize fonts. Raster workers provide immutable
`GlyphRasterSourceRecord` metadata and one byte payload span for cold keys.
The atlas stage verifies those records, performs bounded page placement, and
references the payload without copying it into the submission.

## Working-set contract

`build_glyph_raster_working_set` walks glyph paint commands in paint order and
retains:

- one sorted unique `GlyphRasterKey` per generation, face, glyph, scale,
  raster mode and subpixel phase;
- one `GlyphRasterUseRecord` for every referenced shaped glyph;
- HarfBuzz pen progression and glyph offsets in signed viewport coordinates;
- style, clip, source-line and direction metadata needed by submission.

The shaped glyph array is never flattened or copied. The output cost is
`O(unique viewport glyph keys + viewport glyph uses)`, independent of total
document size.

## Raster identity

A raster key contains:

- immutable font generation ID;
- `FontFaceId`;
- glyph ID;
- exact retained HarfBuzz x/y scale;
- grayscale, LCD or color raster mode;
- bounded x/y subpixel phase.

A generation ID of zero is invalid. Stale font generations cannot alias a
resident raster produced from a different generation.

## Raster-source validation

Cold keys require one matching source record. Sources are sorted by key and
validated for:

- exact semantic-key equality;
- format/mode compatibility;
- bounded dimensions and row byte multiplication;
- payload offset and size containment;
- exact FNV-1a content checksum;
- explicit empty-glyph representation;
- no duplicate-key content collision.

An empty glyph is cached as a semantic result but produces no upload and no
draw instance.

## Atlas cache

`GlyphAtlasCache` owns persistent metadata charged to
`ResourceClass::RasterTile`:

- a fixed maximum page count;
- a fixed page width and height;
- a fixed maximum entry count;
- one shelf allocator per initialized page;
- format-homogeneous pages;
- page-level least-recently-used reset when no existing page can fit;
- page generations and a global atlas generation.

A page reset evicts every entry on that page and increments its generation.
`clear()` increments the global atlas generation. Either operation makes old
submission references detectably stale.

Cache publication is transactional. Candidate page and entry tables are
staged in the bounded metadata resource and swapped into persistent state only
after all keys, sources, limits and output reservations succeed.

## Submission contract

`prepare_glyph_atlas_submission` publishes:

- zero-copy `GlyphAtlasUploadRecord` payload references for cold non-empty
  keys;
- one `GlyphAtlasDrawInstance` per non-empty glyph use;
- maximal consecutive `GlyphAtlasDrawBatch` runs with equal page generation,
  page index, style and clip;
- global and page generation IDs required for stale-reference validation.

Draw positions combine the certified HarfBuzz glyph origin with raster bearing:

- inline start = glyph origin + bearing x;
- block start = baseline origin - bearing y.

The caller supplies hard limits for upload count, upload bytes, draw instances
and draw batches. Output vectors use exact sizing and remain empty after every
failure.

## Compact records

| Record | Bytes |
|---|---:|
| `GlyphRasterKey` | 32 |
| `GlyphRasterWorkingSetEntry` | 48 |
| `GlyphRasterUseRecord` | 48 |
| `GlyphRasterSourceRecord` | 88 |
| `GlyphAtlasPageRecord` | 48 |
| `GlyphAtlasCacheEntry` | 96 |
| `GlyphAtlasUploadRecord` | 64 |
| `GlyphAtlasDrawInstance` | 64 |
| `GlyphAtlasDrawBatch` | 32 |

## Failure model

The stage fails closed for:

- paint/shaping topology violations;
- invalid raster configuration or generation IDs;
- missing, malformed or corrupt raster sources;
- semantic raster-key collisions;
- page or entry capacity exhaustion;
- stale resident metadata;
- arithmetic overflow;
- caller limit violations;
- cache metadata or output PMR budget exhaustion.

Both the output and persistent cache remain unchanged on failure.

## Certification fixture

The fixed benchmark represents a 16,384-line source document and a bounded
80-line paint window:

- 320 glyph uses;
- 96 unique raster keys;
- 64 grayscale, 16 LCD and 16 color keys;
- cold submission: 96 cache misses and uploads;
- hot submission: 96 cache hits and zero uploads;
- 16,896 referenced upload bytes;
- 320 draw instances and 80 backend draw batches;
- 240 coalesced instances;
- exact output current/peak: 43,008 / 58,368 bytes;
- exact cache current/peak: 9,360 / 18,723 bytes;
- deterministic checksum: `5598703025695070182`.

The independent oracle covers 7,644 pen, key identity, use mapping and maximal
batch-grouping cases.

## Explicit boundary

Z2F-2 does not implement FreeType, CoreText or DirectWrite rasterization;
device-pixel transforms; LCD filtering; hinting policy; COLR/CPAL, SVG or
bitmap color-font interpretation; GPU texture creation; API-specific upload
commands; fences; compositor surfaces; image painting; or final backend draw
submission. Those remain later Z2F stages.
