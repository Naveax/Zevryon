# Z1D-C2B-B — Bounded L4 mirroring runtime

## Scope

This stage applies UAX #9 rule L4 after the merged L1-L3 visual-order stage.
It does not rewrite the decoded source stream. Instead, it emits sparse requests
for the shaping layer in ascending visual order.

## Normative decision

A request is emitted only when both conditions hold:

1. the final L1-adjusted embedding level is odd; and
2. Unicode 17 `Bidi_Mirrored` is `Y`.

`Bidi_Mirroring_Glyph` is informative. It is used only to classify the request:

- `ExactCharacter` when a non-best-fit character mapping exists;
- `BestFitCharacter` when the mapping is marked `[BEST FIT]`;
- `MirroredGlyphOnly` when the normative property is set but no character
  mapping exists.

The final case leaves `mirror_codepoint` at zero so a future shaper can request
a mirrored glyph, for example through font features, without inventing a
Unicode character substitution.

## Memory model

Each sparse request is exactly 12 bytes:

- 4-byte visual index;
- 4-byte optional mirror codepoint;
- 1-byte request kind;
- 3 reserved bytes.

The runtime performs a validation/count pass followed by one exact `reserve`.
There is no geometric vector growth, copied source stream, inverse visual map,
or dense per-scalar flag buffer. Output is swapped into place only after the
entire operation succeeds.

## Stage boundary

The C2A visual order is a trusted stage contract. C2B-B validates the fields it
consumes: active stream sizes, strictly increasing X9-active unit indices,
codepoint links, final level bounds, and visual index bounds. The permutation
proof remains owned by C2A, which starts from identity and uses only bounded
reversals.

Shaping, glyph fallback, caret maps, accessibility hit testing, and painting are
outside Z1D.
