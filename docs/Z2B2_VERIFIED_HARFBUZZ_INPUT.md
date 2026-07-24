# Z2B-2 — Verified HarfBuzz font-input boundary

## Scope

Z2B-2 connects the merged allocation-free SFNT/TTC parser and integrity
verifier to the optional HarfBuzz shaping backend. Caller-owned font bytes can
no longer reach `hb_blob_create` until they pass both Zevryon validation gates.

The public shaping API remains free of HarfBuzz types. The caller still owns and
keeps the source bytes immutable for the duration of one synchronous shaping
call.

## Execution order

`shape_harfbuzz_segment` now performs:

1. reset output, statistics, and error state;
2. open the requested single-font or TTC/OTC face with `open_sfnt_resource`;
3. apply strict `verify_sfnt_integrity` policy;
4. create the read-only HarfBuzz blob and selected face;
5. validate shaping input, configure font/buffer/features/variations, and shape;
6. validate clusters and glyph-position output;
7. publish one exactly reserved glyph run and combined validation/shaping stats.

A structural or integrity failure returns before any HarfBuzz object is created.

## Private backend split

The original shaping implementation is compiled as the private symbol
`shape_harfbuzz_segment_backend`. A small public wrapper owns the security
boundary and calls the private backend only after successful validation.

This keeps the established shaping implementation and benchmark behavior
isolated while making it impossible for public callers to bypass the verifier.
The private symbol is not declared in the public header.

## Error propagation

Font failures retain the existing public `InvalidFontData` category and add:

- exact SFNT parser sub-error;
- exact integrity-verifier sub-error;
- source byte offset;
- affected table tag where available.

Structural failure leaves the integrity sub-error empty. Integrity failure
leaves the parser sub-error empty. Backend allocation, shaping, output, and
budget errors continue to use their existing categories.

## Statistics

Successful shaping adds:

- validated face count;
- validated table count;
- verified table-checksum count;
- verified payload and padding bytes;
- standalone whole-font checksum status;
- explicit collection whole-checksum skip status.

These fields are published atomically with the existing glyph and shaping
statistics.

## Validation

The focused verified-input test uses the real fixed DejaVu Sans fixture and
requires:

- strict structural and integrity validation before successful Latin shaping;
- every parsed table checksum verified;
- standalone whole-font checksum verified;
- unchanged non-empty glyph output;
- no parser or integrity sub-error on success;
- one corrupted `cmap` payload byte rejected with exact table tag and offset;
- arbitrary non-SFNT bytes rejected with exact parser sub-error;
- a single-font face index greater than zero rejected before backend creation;
- no glyph or shaping-stat publication after rejected font input.

The existing Latin, Arabic, Hebrew, combining-mark, and Devanagari shaping tests
remain unchanged and must continue to pass. The uncached full-call benchmark now
includes the mandatory parser and integrity cost on every measured shaping call.

## Explicit boundary

Z2B-2 does not:

- cache a previously verified font resource;
- map discovery paths or watch files;
- parse semantic `cmap`, metrics, variation, color, or layout-table fields;
- replace HarfBuzz table parsing with custom shaping logic;
- rasterize, cache glyph images, or paint.

A later resource-cache layer may retain immutable verified font generations and
avoid repeating whole-file integrity work for every segment, while preserving
this public fail-closed boundary.
