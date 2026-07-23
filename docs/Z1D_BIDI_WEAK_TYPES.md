# Z1D-B2 — Bounded weak-type resolution

Z1D-B2 resolves Unicode 17 UAX #9 rules W1-W7 independently for every
isolating run sequence produced by Z1D-B1.

## Implemented scope

- W1 nonspacing-mark inheritance, including isolate-initiator / `PDI` handling;
- W2 European-number conversion after Arabic letters;
- W3 Arabic-letter conversion to right-to-left;
- W4 numeric separator conversion;
- W5 European terminator sequence conversion;
- W6 neutralization of remaining separators and terminators;
- W7 European-number conversion after left-to-right strong text;
- immutable explicit units and isolating-run topology;
- one-byte resolved type per X9-active scalar;
- sequence traversal without materializing a duplicate sequence-index array;
- fail-closed validation of active indices, level runs, sequence links, and
  sequence coverage;
- publish-on-success output and dedicated `BidiTypeResolution` hard budget;
- rule-specific counters, adversarial unit tests, rule fixtures, strict
  cross-platform builds, and sanitizer coverage.

## Data and memory contract

Output index `i` corresponds to
`BidiSequenceTopology::active_unit_indices[i]`. The output stores only one
`BidiClass` byte per active scalar. Topology validation temporarily stores one
visited byte per level run. No source text, explicit unit, or sequence member
list is copied.

The certification workload uses a 64 KiB hard cap for output plus validation
state. Failed validation or allocation publishes no partial output and releases
all temporary bytes.

## Correctness boundary

This stage resolves weak types only. It does not implement:

- paired brackets N0;
- neutral resolution N1-N2;
- implicit levels I1-I2;
- L1 whitespace reset;
- L2 visual reordering;
- mirroring or glyph-facing visual order.

Z2 shaping must not consume Z1D-B2 output as final reordered text.
