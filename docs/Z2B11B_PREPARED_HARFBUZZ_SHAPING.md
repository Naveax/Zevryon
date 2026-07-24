# Z2B-11B Prepared HarfBuzz Face Shaping

## Purpose

Z2B-11A retains one immutable `hb_blob_t` and `hb_face_t` over a verified
catalog font resource. Z2B-11B connects that prepared face to the existing
HarfBuzz shaping backend so repeated short runs do not rebuild the native blob
and face on every call.

```text
CatalogFontFaceBinding
  -> PreparedHarfBuzzFace (one cold hb_blob + immutable hb_face)
  -> per-call hb_font + hb_buffer
  -> ShapedGlyphRun
```

## Input contract

`HarfBuzzShapingRequest` supports exactly one font-input mode:

1. raw font bytes, with inline SFNT/TTC parse and integrity verification;
2. `VerifiedFontResource`, skipping repeat verification;
3. `PreparedHarfBuzzFace`, additionally skipping `hb_blob_create` and
   `hb_face_create`.

Supplying zero modes or more than one mode fails before backend work. A
prepared request must use the face index retained by the prepared object.

## Ownership and isolation

The public wrapper retains the prepared object through the complete synchronous
call. The backend borrows its immutable native `hb_face_t` only while that
handle is alive. It still creates a fresh `hb_font_t` and `hb_buffer_t` for
every call, so scale and variation coordinates remain request-local and
concurrent shaping calls cannot mutate shared face state.

The prepared path:

- performs no file access, content hashing, cache lookup, SFNT parsing, checksum
  verification, `hb_blob_create`, or `hb_face_create`;
- validates that the prepared face and retained verified bytes describe the
  same resource and selected face;
- preserves the existing cluster, feature, variation, output, overflow, and
  memory-budget checks;
- publishes `used_prepared_harfbuzz_face=true` while preserving the verified
  resource identity and validation statistics;
- clears prior glyph output before validation and publishes only after complete
  shaping success.

## Required certification

The real DejaVu Sans tests prove:

- prepared and verified-resource shaping are byte-exact equivalents;
- glyph count, UPEM, resource identity, and validation evidence remain exact;
- different per-call scales do not leak through the immutable shared face;
- repeating an earlier scale after another call is byte-exact;
- mixed font-input modes and face-index mismatches fail atomically;
- a one-byte glyph budget publishes no output;
- all glyph ledgers remain clean;
- strict GCC and Linux ASan/UBSan pass.

The dedicated short-run benchmark alternates verified and prepared batches in
the same process. Across repeated workflow executions it requires:

- identical input/output counts, advances, UPEM, resource identity, and memory
  accounting;
- the prepared mode flag only on the prepared path;
- every prepared/verified P50 ratio at or below `1.10`;
- median prepared/verified P50 ratio at or below `0.97`.

## Explicit boundary

This slice does not retain or share `hb_font_t`, `hb_buffer_t`, variation state,
scale state, feature arrays, or shape plans. It does not add a prepared-face
LRU, asynchronous preparation, fallback selection, multi-run shaping, caret
mapping, line breaking, rasterization, or painting.
