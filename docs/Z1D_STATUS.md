# Z1D status

- Z1D-A: implemented — Unicode 17 `Bidi_Class`, paragraph direction, X1-X8
  explicit embedding/override/isolate processing, bounded output, and
  full-codepoint property conformance.
- Z1D-B1: implemented — virtual X9 filtering, level runs, BD13 isolating run
  sequences, and X10 `sos`/`eos` under a 1 MiB certified hard cap.
- Z1D-B2: implemented — W1-W7 weak-type resolution with bounded PMR output,
  focused regression coverage, and the selected Unicode 17 `BidiTest.txt`
  conformance path.
- Z1D-B3: implemented — N0 paired brackets, N1-N2 neutral resolution, and I1-I2
  implicit levels with bounded resource-ledger accounting and focused tests.
- Z1D-C2A: implemented — L1 reset, L2 visual reordering, and L3 combining-mark
  repair with bounded visual-order output and Unicode 17 `BidiTest.txt` subset
  coverage.
- Z1D-C2B: implemented — L4 mirroring requests using pinned Unicode 17
  `Bidi_Mirrored` / mirroring-glyph authority, including exact, best-fit, and
  glyph-only outcomes.

## Full-pipeline authority closure

The existing `zevryon-bidi-visual-conformance` executable retains its historical
bounded `BidiTest.txt` subset mode and also supports:

```text
zevryon-bidi-visual-conformance --character-test BidiCharacterTest.txt
```

The character-test mode drives real Unicode scalars through the complete current
pipeline rather than constructing already-resolved synthetic units:

```text
Bidi_Class lookup
  -> paragraph direction + X1-X8
  -> virtual X9 filtering
  -> BD13/X10 isolating-run sequences
  -> W1-W7
  -> N0-N2
  -> I1-I2
  -> L1-L3 visual order
```

For every `BidiCharacterTest.txt` case it checks the resolved paragraph level,
all original-position levels including `x` entries removed by X9, and visual
reorder indices mapped back to original code-point positions. Every allocation
stage is executed behind an explicit `ResourceLedger` hard cap.

## Normative L2/L3 boundary

Unicode 17 `BidiCharacterTest.txt` defines its reorder field through rule L2
inclusively. L3 and L4 are intentionally outside that corpus because they are
rendering-dependent. Zevryon's production visual-order API deliberately applies
L3 after L2, so comparing its final order directly to the corpus would create a
false mismatch whenever a right-to-left combining sequence requires L3 repair.

`scripts/z1_bidi_character_l3_adapter.py` bridges only that specified boundary:

- the original Unicode 17 corpus remains byte-for-byte pinned by SHA-256;
- every normative case remains in the denominator;
- paragraph levels and resolved scalar levels are untouched;
- only the normative L2 reorder field is advanced through the same L3 combining
  sequence transformation exposed by the production visual-order surface;
- Unicode 17 NSM classification comes from the repository's generated
  `Bidi_Class` authority, not the host Python Unicode database;
- no mismatch, character class, or test case is filtered or xfailed.

L4 mirroring remains separately covered because the character conformance file
defines levels/order, not glyph substitution.

The runner and adapter being present are not themselves certification credit.
Z1D remains active until the exact branch/head is compiled on the supported
Windows/Linux contract and the full pinned Unicode 17 character corpus completes
with zero mismatches.
