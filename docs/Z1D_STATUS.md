# Z1D status

- Z1D-A: implemented — Unicode 17 `Bidi_Class`, paragraph direction, X1-X8
  explicit embedding/override/isolate processing, bounded output, and
  full-codepoint property conformance.
- Z1D-B1: implemented — virtual X9 filtering, level runs, BD13 isolating run
  sequences, and X10 `sos`/`eos` under a 1 MiB certified hard cap.
- Z1D-B2: not implemented — W1-W7 weak type resolution.
- Z1D-B3: not implemented — paired brackets N0, N1-N2 neutral resolution, and
  I1-I2 implicit levels.
- Z1D-C: not implemented — L1 whitespace reset, L2 visual reordering, mirroring,
  and glyph-facing integration.

Z1D remains active until Z1D-B2/B3, Z1D-C, and full `BidiTest.txt` /
`BidiCharacterTest.txt` conformance are complete.
