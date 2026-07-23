# Z1D-A — Bounded Unicode 17 bidi substrate

Z1D-A adds the first bidirectional-text layer above the UTF-8, grapheme, and
script substrates.

## Implemented

- deterministic Unicode 17.0.0 `Bidi_Class` table generation with `@missing`
  coverage;
- full-codepoint property lookup and conformance tooling;
- automatic or forced paragraph base level;
- paragraph first-strong detection that ignores isolate contents;
- O(n) `FSI` first-strong resolution with bounded fixed-depth state;
- X1-X8 explicit embedding, override, and isolate stack processing;
- overflow counters and a hard maximum explicit level of 125;
- 16-byte per-codepoint explicit units;
- X9-removal flags for `LRE`, `RLE`, `LRO`, `RLO`, `PDF`, and `BN`;
- separate flags for isolate initiators and `PDI`;
- post-pop level assignment for retained `PDF` and `PDI` units;
- outer-override resolution for retained `PDI` units;
- paragraph-level assignment for a final paragraph separator;
- `BN` preservation under active directional overrides;
- `BidiRun` Resource Ledger accounting and hard-budget rejection;
- cross-platform strict tests, Linux sanitizer coverage, focused AppleClang
  diagnostics, and dedicated MSVC ASan.

## Unit contract

`BidiExplicitUnit.level` is the explicit embedding level assigned by UAX #9
X1-X8. Retained embedding and isolate initiators use the surrounding pre-push
level. Retained `PDF` and `PDI` units use the post-pop level. `resolved_class`
contains an active override where UAX #9 applies it; `BN` and X9 formatting
controls are not rewritten by an override.

Later Z1D stages discard units flagged `kBidiUnitRemovedByX9`. The
implementation emits exactly one unit per decoded scalar, does not copy source
text, and preserves absolute source offsets.

The API accepts exactly one paragraph. A paragraph separator, when present,
must be the final scalar and is assigned the paragraph embedding level.

## Certified evidence

Unicode 17 `Bidi_Class` property conformance covers all 1,114,112 codepoints:

- explicit source ranges: 2,323;
- `@missing` ranges: 24;
- generated compact ranges: 1,267;
- property mismatches: 0.

The 64 KiB mixed-direction workload contains 35,502 decoded codepoints and is
measured over three independent 1,024-iteration distributions after 32 warmup
iterations:

- median P50: 1.30533 ms;
- median P95: 1.33305 ms;
- median P99: 1.36172 ms;
- worst maximum: 1.45620 ms;
- current `BidiRun` allocation: 568,032 bytes;
- worst peak allocation: 603,534 bytes;
- hard cap: 1,048,576 bytes;
- rejected reservations: 0;
- accounting errors: 0.

## Explicit boundary

This is not the complete Unicode Bidirectional Algorithm. The following remain
for Z1D-B/C:

- isolating run sequence construction;
- weak type resolution W1-W7;
- paired bracket algorithm N0;
- neutral resolution N1-N2;
- implicit levels I1-I2;
- whitespace reset L1;
- visual reordering L2;
- mirroring and glyph-facing integration;
- complete `BidiTest.txt` and `BidiCharacterTest.txt` conformance.

Z2 shaping must not consume Z1D-A output as final visual order.
