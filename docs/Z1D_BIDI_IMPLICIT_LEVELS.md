# Z1D-C1 — Bounded Unicode 17 implicit embedding levels

Z1D-C1 implements Unicode 17 UAX #9 revision 51 rules I1 and I2 after the
explicit, isolating-sequence, weak-type, bracket, and neutral stages merged in
Z1D-A through Z1D-B3.

## Implemented scope

- I1 on even embedding levels:
  - `R` receives level +1;
  - `EN` and `AN` receive level +2;
  - `L` remains unchanged;
- I2 on odd embedding levels:
  - `L`, `EN`, and `AN` receive level +1;
  - `R` remains unchanged;
- explicit input levels are restricted to the UAX #9 `max_depth` range 0-125;
- implicit output is restricted to `max_depth + 1`, or level 126;
- one resolved level byte per X9-active scalar;
- immutable explicit units, sequence topology, and N0-N2 type output;
- publish-on-success output with no partial level vector after failure;
- dedicated `BidiImplicitLevel` Resource Ledger accounting;
- the new resource class is appended without changing existing ResourceClass
  ordinal values.

## Stage contract

The input type stream must already have completed W1-W7 and N0-N2. Therefore,
only `L`, `R`, `EN`, and `AN` are accepted. Any remaining weak, neutral,
formatting, isolate, or explicit type is rejected as an invalid stage input.

Output index `i` corresponds to
`BidiSequenceTopology::active_unit_indices[i]`. Source code points and units
removed by X9 are not copied into the output.

## Topology validation

Before producing levels, the resolver verifies:

- active index count fits the 32-bit topology representation;
- active indices are strictly increasing and address valid explicit units;
- no active unit is flagged as removed by X9;
- explicit levels and level-run levels are at most 125;
- level runs form a contiguous partition of the active stream;
- explicit-unit levels agree with their level-run descriptors;
- sequence links cover every run exactly once;
- sequence links remain in logical order;
- sequence levels agree with every referenced run;
- `sos` and `eos` are valid strong directions.

Run coverage uses a bit-packed temporary array of one bit per level run rather
than one byte per run.

## Normative conformance

The final workflow pins Unicode 17.0.0 `BidiTest.txt` with SHA-256:

```text
888bdfc8090652272d1f859cdb00ae659e2dc6c26740be61ef1d03998a687620
```

Because Z1D-C1 intentionally does not yet implement L1-L4, the conformance
runner selects a bounded official subset that does not require explicit
controls, isolates, paragraph separators, segment separators, whitespace reset,
or visual reordering. Each selected case is still processed through the real
pipeline:

```text
initial Bidi_Class
→ W1-W7
→ N0-N2
→ I1-I2
```

Final normative results:

- total `BidiTest.txt` data lines: 490,846;
- selected lines: 21,093;
- total paragraph-mode cases: 33,336;
- automatic-LTR cases: 11,112;
- forced-LTR cases: 11,112;
- forced-RTL cases: 11,112;
- failures: 0.

## Memory and performance certification

The benchmark constructs a 64 KiB mixed-direction paragraph and executes the
real explicit → sequence → weak → neutral → implicit pipeline. Only the I1-I2
stage is timed.

Final three-distribution certification:

- input code points: 42,325;
- X9-active scalars and output levels: 39,595;
- isolating run sequences: 4,097;
- level record size: 1 byte;
- I1 `R` changes: 9,556;
- I1 number changes: 6,827;
- I2 `L` changes: 4,097;
- I2 number changes: 4,096;
- median P50: 0.215726 ms;
- median P95: 0.241747 ms;
- median P99: 0.276211 ms;
- worst observed sample: 0.333909 ms;
- peak and resident implicit-level memory: 39,595 bytes;
- hard cap: 49,152 bytes (48 KiB);
- rejected reservations: 0;
- accounting errors: 0.

Permanent certification gates require median P95 ≤0.40 ms, median P99 ≤0.50
ms, worst sample ≤1.00 ms, and peak memory ≤48 KiB.

## Cross-platform gates

Z1D-C1 requires:

- strict GCC build and tests;
- strict AppleClang build and tests;
- dedicated Windows MSVC AddressSanitizer;
- Ubuntu, Windows, and macOS main CI;
- Linux ASan/UBSan;
- the complete Windows sanitizer matrix;
- all existing ZENITH, UTF-8, grapheme, script, explicit-bidi,
  isolating-sequence, weak-type, and neutral-stage regressions.

## Explicit boundary

Z1D-C1 does **not** implement:

- L1 paragraph/segment separator and whitespace level reset;
- L2 visual reordering;
- L3 combining-mark visual handling;
- L4 mirroring;
- final logical-to-visual index maps or glyph-facing order.

Those operations belong to Z1D-C2. Z2 shaping must not consume Z1D-C1 levels
as final visual text order.
