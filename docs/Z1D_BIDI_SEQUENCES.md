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
- UAX #9 X10 examples, nested/unmatched/empty isolate tests, strict
  cross-platform builds, and sanitizer coverage.

## Data model

The canonical explicit-unit array remains owned by Z1D-A. Z1D-B1 stores only:

- `active_unit_indices`: original-unit indices surviving X9;
- `level_runs`: contiguous equal-level slices over the active index array;
- `sequence_run_indices`: the ordered level-run IDs in each isolating sequence;
- `sequences`: compact descriptors with level, `sos`, and `eos`.

No source text or explicit unit is duplicated.

## Memory contract

- active-unit index: 4 bytes;
- level-run descriptor: at most 12 bytes;
- level-run link: 4 bytes;
- sequence descriptor: at most 16 bytes;
- all output and temporary topology vectors share the `BidiSequence` hard
  budget;
- a failed construction publishes no partial topology and releases all
  temporary bytes.

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
shaping-facing visual cluster output.
