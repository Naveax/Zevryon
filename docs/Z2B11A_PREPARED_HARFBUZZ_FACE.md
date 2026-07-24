# Z2B-11A Immutable Prepared HarfBuzz Face

## Purpose

Z2B-10 removes catalog lookup, filesystem I/O, hashing, and verified-resource
cache activity from repeated shaping. The backend still constructs a HarfBuzz
blob and face on every shaping call. Z2B-11 is divided into two reviewable
slices:

- **Z2B-11A:** prepare and retain one immutable `hb_blob_t + hb_face_t` pair;
- **Z2B-11B:** move shaping onto a common backend that consumes the prepared
  face and avoids rebuilding native face state per segment.

This document covers Z2B-11A only.

## Ownership model

`prepare_harfbuzz_face` accepts an already valid `CatalogFontFaceBinding` and
publishes a shared immutable `PreparedHarfBuzzFace` only after:

- the verified resource exposes non-empty HarfBuzz-compatible bytes;
- a read-only HarfBuzz blob is created over those retained bytes;
- the selected sfnt/TTC face is created;
- glyph count and units-per-em are non-zero and bounded;
- the native face is made immutable;
- final retained ownership validation succeeds.

The prepared object owns a copy of the catalog binding. Therefore it retains:

- the immutable catalog generation;
- the verified font resource and its source bytes;
- the generation-local face ID;
- the HarfBuzz blob and immutable face.

The native destruction order is face first, blob second. A failed preparation
clears any previous output and destroys all partially created native objects.

## Cache and I/O boundary

Preparation consumes only the bytes already retained by the binding. It does
not parse a platform identity, convert a path, read a file, hash content, query
the verified-resource cache, or rescan SFNT integrity.

The focused test binds a real DejaVu face, releases the caller's generation
handle, clears the verified-resource cache, and then prepares the native face.
The complete cache snapshot must remain unchanged.

## Certification

The test requires:

- exact generation, face, resource, font-byte, glyph-count, and UPEM metadata;
- non-null native face state;
- immutable-face creation evidence;
- validity after caller binding ownership is released;
- unchanged verified-resource cache state during preparation;
- invalid/default binding rejection;
- failure-atomic clearing of a previously valid prepared face;
- strict GCC and Linux ASan/UBSan.

## Explicit boundary

Z2B-11A does not yet shape through the prepared face, retain `hb_font_t`, cache
variation/scaling instances, cache explicit shape plans, maintain an LRU of
prepared faces, estimate HarfBuzz native heap usage, perform asynchronous
construction, rasterize glyphs, or paint text. Z2B-11B will refactor the backend
to consume the immutable face while keeping per-request font and buffer state
isolated.
