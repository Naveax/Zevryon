# Z1D-B1 — Bounded X9/X10 bidi sequence topology

Z1D-B1 converts Z1D-A explicit units into the topology required by later UAX
#9 weak, neutral, bracket, and implicit-level stages.

## Implemented scope

- virtual X9 filtering without copying `BidiExplicitUnit` records;
- stable mapping from filtered positions to original explicit-unit indices;
- matching isolate-initiator / `PDI` discovery;
- contiguous level-run construction over the X9-filtered stream;
- BD13 isolating-run-sequence linking through matching `PDI` runs;
- X10 `sos` and `eos` computation from the original explicit levels;
- paragraph fallback for end-of-paragraph and unmatched isolate initiators;
- compact level-run and sequence descriptors;
- dedicated `BidiSequence` Resource Ledger hard budget;
- fail-closed validation of flags, unit order, levels, paragraph boundaries,
  sequence graph uniqueness, and reachability;
- publish-on-success topology construction with no partial state on failure;
- official Unicode 17 BD13 Example 1–3 conformance;
- nested, unmatched, empty-isolate, invalid-input, and hard-budget tests;
- GCC, AppleClang, MSVC, Linux ASan/UBSan, and MSVC ASan coverage.

## Data model

The canonical explicit-unit array remains owned by Z1D-A. Z1D-B1 stores only:

- `active_unit_indices`: original-unit indices surviving X9;
- `level_runs`: contiguous equal-level slices over the active index array;
- `sequence_run_indices`: the ordered level-run IDs in each isolating sequence;
- `sequences`: compact descriptors with level, `sos`, and `eos`.

No source text or explicit unit is duplicated. Matching isolate state and graph
validation tables are temporary and share the same hard budget.

## Physical memory contract

- active-unit index: 4 bytes;
- level-run descriptor: 12 bytes;
- level-run link: 4 bytes;
- sequence descriptor: 16 bytes;
- all output and temporary topology vectors share a 1 MiB certification cap;
- a failed construction publishes no partial topology and releases all
  temporary bytes.

A redundant active-position-to-run table and a worst-case isolate-stack reserve
were removed after the first benchmark. This reduced certified peak allocation
from 1,107,298 bytes to 860,804 bytes while also improving latency.

## Certified evidence

Official Unicode 17.0.0 UAX #9 revision 51 BD13 examples:

- examples: 3 / 3 PASS;
- expected isolating run sequences: 11 / 11;
- sequence-member, level, `sos`, and `eos` mismatches: 0.

64 KiB mixed embedding/isolate workload, three independent distributions of
1,024 measured iterations after 32 warmups:

- input codepoints / explicit units: 33,258;
- active units after X9: 26,411;
- removed units: 6,847;
- level runs: 11,738;
- isolating run sequences: 8,804;
- matching isolate pairs: 2,934;
- median P50: 0.453711 ms;
- median P95: 0.529626 ms;
- median P99: 0.554732 ms;
- worst maximum: 0.782087 ms;
- current allocation: 684,724 bytes;
- worst peak allocation: 860,804 bytes;
- hard cap: 1,048,576 bytes;
- rejected reservations: 0;
- accounting errors: 0.

## Correctness boundary

This milestone constructs topology only. It does not mutate resolved bidi
classes or levels.

The following remain for later Z1D-B stages:

- W1-W7 weak type resolution;
- paired-bracket algorithm N0;
- N1-N2 neutral resolution;
- I1-I2 implicit levels;
- complete `BidiTest.txt` and `BidiCharacterTest.txt` conformance.

Z1D-C still owns L1 whitespace reset, L2 visual reordering, mirroring, and
shaping-facing visual cluster output. Z2 shaping must not consume Z1D-B1
output as final reordered text.
