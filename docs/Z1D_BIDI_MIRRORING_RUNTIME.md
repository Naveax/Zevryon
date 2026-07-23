# Z1D-C2B-B — Bounded L4 mirroring runtime

## Scope

This stage applies Unicode 17 UAX #9 revision 51 rule L4 after the merged L1-L3
visual-order stage. It never rewrites the decoded source stream. Instead, it
emits sparse requests for the shaping layer in ascending visual order.

## Normative decision

A request is emitted only when both conditions hold:

1. the final L1-adjusted embedding level is odd; and
2. Unicode 17 `Bidi_Mirrored` is `Y`.

`Bidi_Mirroring_Glyph` is informative and only classifies the request:

- `ExactCharacter` when a non-best-fit character mapping exists;
- `BestFitCharacter` when the mapping is marked `[BEST FIT]`;
- `MirroredGlyphOnly` when the normative property is set but no character
  mapping exists.

The glyph-only case leaves `mirror_codepoint` at zero so a future shaper can
request a mirrored glyph without inventing a Unicode character substitution.

## Pinned Unicode 17 data

- `UnicodeData.txt` SHA-256:
  `2e1efc1dcb59c575eedf5ccae60f95229f706ee6d031835247d843c11d96470c`
- `BidiMirroring.txt` SHA-256:
  `a2f16fb873ab4fcdf3221cb1a8a85a134ddd6ed03603181823ff5206af3741ce`
- normalized table fingerprint:
  `12477c826aad16df4fff77df606e37b69a194a3f53b1778337ad5881338fa9b9`
- normative mirrored codepoints: 554;
- exact character mappings: 354;
- best-fit mappings: 74;
- mirrored codepoints without character mapping: 126.

The runtime conformance executable scans all 1,114,112 Unicode codepoint values
and compares the committed lookup tables directly with both pinned sources.

## Memory model

Each sparse request is exactly 12 bytes:

- 4-byte visual index;
- 4-byte optional mirror codepoint;
- 1-byte request kind;
- 3 reserved bytes.

The runtime performs a validation/count pass followed by one exact `reserve`.
The count pass uses only the normative mirrored-property range table; mapping
lookup occurs only when writing an actual request. Visual-index bounds are
validated in that count pass before any level or unit dereference.

There is no geometric vector growth, copied source stream, inverse visual map,
or dense per-scalar flag buffer. Output and statistics are published only after
the complete operation succeeds.

## Final 64 KiB certification

- input codepoints / active visual units: 47,662;
- odd-level units: 29,789;
- mirrored property hits / output requests: 8,937;
- exact requests: 2,979;
- best-fit requests: 2,979;
- glyph-only requests: 2,979;
- resident and peak output: 107,244 bytes;
- hard cap: 110,592 bytes / 108 KiB;
- median P50: 0.795692 ms;
- median P95: 0.824076 ms;
- median P99: 0.874331 ms;
- worst maximum: 1.38042 ms;
- rejected reservations: 0;
- accounting errors: 0.

Permanent gates are P95 <= 1.10 ms, P99 <= 1.50 ms, worst <= 3.00 ms, and peak
<= 108 KiB across three independent distributions.

## Stage boundary

The C2A visual order is a trusted stage contract. C2B-B validates the fields it
consumes: active stream sizes, strictly increasing X9-active unit indices,
codepoint links, final level bounds, and visual index bounds. The permutation
proof remains owned by C2A, which starts from identity and uses only bounded
reversals.

Shaping, font fallback, caret maps, accessibility hit testing, and painting are
outside Z1D.
