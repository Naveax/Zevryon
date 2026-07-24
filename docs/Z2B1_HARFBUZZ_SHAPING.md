# Z2B-1 — bounded HarfBuzz segment shaping

## Scope

Z2B-1 establishes the first real glyph-shaping boundary above the completed
Unicode, grapheme, Script, bidi, font fallback, and native font-discovery
layers.

The portable Zevryon core does not link HarfBuzz. When HarfBuzz is available,
the optional backend shapes one already-segmented font, Script, direction, and
language run into compact glyph records charged to the existing `GlyphRun`
Resource Ledger class.

## Input contract

One shaping call receives:

- caller-owned immutable font bytes and a face index;
- the complete decoded codepoint and grapheme-boundary streams;
- one non-empty half-open grapheme-cluster range;
- one resolved Unicode Script;
- one text direction;
- one BCP 47 language;
- global OpenType feature settings;
- optional variable-font coordinates;
- optional explicit X/Y font scales;
- beginning/end-of-text and unsafe-to-concat production flags.

The caller must split text at font fallback, Script, direction, language,
feature, and variation boundaries before calling the backend. Z2B-1 does not
silently guess those properties.

## HarfBuzz object boundary

The backend:

1. wraps caller-owned bytes in an `HB_MEMORY_MODE_READONLY` blob;
2. creates one face for the supplied face index;
3. creates one HarfBuzz font using native OpenType font functions;
4. applies scale and variation coordinates;
5. creates one Unicode buffer;
6. sets explicit direction, Script, language, flags, and monotone-grapheme
   cluster behavior;
7. adds every input scalar with its Zevryon grapheme-cluster index;
8. calls `hb_shape_full` with the requested global features;
9. validates the complete glyph-info and glyph-position arrays;
10. destroys every HarfBuzz object before returning.

The read-only blob never owns or modifies the caller's font bytes. The bytes
must remain alive only for the duration of the synchronous shaping call.

## Output contract

Each shaped glyph is exactly 28 bytes:

- 32-bit glyph identifier;
- 32-bit global grapheme-cluster index;
- signed 32-bit X/Y advances;
- signed 32-bit X/Y offsets;
- 32-bit Zevryon glyph flags.

The flags preserve HarfBuzz unsafe-to-break, unsafe-to-concat, and
safe-to-insert-tatweel information when produced by the installed backend.
Glyph identifier zero is counted explicitly as missing coverage.

A successful call records:

- input codepoint and cluster counts;
- output glyph count;
- missing and safety-flag counts;
- signed 64-bit total advances;
- maximum absolute glyph offset;
- face glyph count, font bytes, and units per em.

The output carries the shaped cluster range, face index, Script, direction, and
actual X/Y scale.

## Atomic memory behavior

HarfBuzz's temporary face, font, buffer, and internal shaping allocations are
short-lived backend memory. Persistent Zevryon output is separate.

After HarfBuzz succeeds, the backend reads the exact glyph count and performs
one exact reserve in the caller-provided PMR resource. The completed temporary
`ShapedGlyphRun` is swapped into the caller output only after every returned
cluster and position has been validated.

Invalid input, invalid font data, HarfBuzz allocation or shaping failure,
invalid backend output, integer overflow, or `GlyphRun` hard-budget rejection
publishes no glyphs and resets all output metadata.

## Cluster policy

Every scalar in one already-resolved extended grapheme cluster is added with
the same global cluster index. The buffer uses
`HB_BUFFER_CLUSTER_LEVEL_MONOTONE_GRAPHEMES`.

This keeps fallback, line breaking, hit testing, and future caret mapping tied
to Zevryon's UAX #29 grapheme boundaries while still allowing HarfBuzz to
merge, split, substitute, and reorder glyphs. Left-to-right output cluster
values remain monotone increasing; right-to-left output remains monotone
decreasing.

## Certification

The focused Linux workflow installs a fixed HarfBuzz development package and
fixed DejaVu/Noto test fonts. Strict GCC and Linux AddressSanitizer plus
UndefinedBehaviorSanitizer run real shaping for:

- Latin standard ligatures;
- a combining-mark grapheme;
- Arabic right-to-left shaping;
- Hebrew right-to-left shaping;
- Devanagari conjunct shaping;
- invalid font data;
- one-byte `GlyphRun` hard-budget rejection.

Repeated Latin shaping must produce byte-identical glyph records and matching
statistics. The successful output allocation must equal exactly
`glyph_count * sizeof(ShapedGlyph)`.

## Explicitly not implemented

Z2B-1 does not:

- load files from discovery identities;
- mmap or cache font binaries;
- cache `hb_face_t`, `hb_font_t`, or shape plans;
- derive document language, features, or variation coordinates;
- merge adjacent shaped segments;
- synthesize missing-glyph fallback after shaping;
- break lines, create caret maps, perform accessibility hit testing, rasterize
  glyphs, or paint text;
- prove parity with DirectWrite, CoreText, or browser rendering output.

Those remain later Z2 partitions. This PR proves the bounded portable shaping
contract and one real HarfBuzz backend, not the complete browser text stack.
