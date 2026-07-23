# Z1D-A — Bounded Unicode 17 bidi substrate

Z1D-A adds the first bidirectional-text layer above the UTF-8, grapheme, and
script substrates.

## Implemented

- deterministic Unicode 17.0.0 `Bidi_Class` table generation;
- full-codepoint property lookup and conformance tooling;
- automatic or forced paragraph base level;
- paragraph first-strong detection that ignores isolate contents;
- O(n) `FSI` first-strong resolution with bounded fixed-depth state;
- explicit embedding, override, and isolate stack processing;
- overflow counters and a hard maximum explicit level of 125;
- 16-byte per-codepoint explicit units;
- X9-removal flags for `LRE`, `RLE`, `LRO`, `RLO`, `PDF`, and `BN`;
- separate flags for isolate initiators and `PDI`;
- `BidiRun` Resource Ledger accounting and hard-budget rejection;
- cross-platform strict tests, Linux sanitizer coverage, and dedicated MSVC ASan.

## Unit contract

`BidiExplicitUnit.level` is the embedding level active immediately before the
scalar is processed. `resolved_class` includes an active LRO/RLO override.
Later Z1D stages may remove units flagged `kBidiUnitRemovedByX9`.

The implementation emits exactly one unit per decoded scalar. It does not copy
source text and preserves absolute source offsets.

## Explicit boundary

This is not the complete Unicode Bidirectional Algorithm. The following remain
for Z1D-B/C:

- isolating run sequence construction;
- weak type resolution W1–W7;
- paired bracket algorithm N0;
- neutral resolution N1–N2;
- implicit levels I1–I2;
- whitespace reset L1;
- visual reordering L2;
- mirroring and glyph-facing integration;
- complete `BidiTest.txt` and `BidiCharacterTest.txt` conformance.

Z2 shaping must not consume Z1D-A output as final visual order.
