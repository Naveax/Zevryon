# M8 mixed-mutation integrity contract

## Authority

Machine-readable report schema: `zevryon.m8.mixed-mutation.v1`.

Authority: `m8-sequence-mixed-mutation-integrity-v1`.

The target under test is the production `ChunkedOrderStatisticsSequence` mutation surface. A deterministic `std::vector<SequenceRecord>` oracle is maintained independently and compared against the live sequence throughout the run.

## Required mutation classes

The harness mixes all five production mutation classes:

- insert;
- erase;
- move;
- height update;
- search-summary update.

The workset stays intentionally bounded while the operation count grows. M8 is testing long-running mutation correctness, copy-on-write snapshot survival and sequence invariants, not rewarding a harness for exhausting the runner with an ever-growing reference vector.

## Certification threshold

Final mutation certification requires **at least 10,000,000 completed mixed mutations** in one admitted run.

`--certification` with fewer than `10,000,000` operations is invalid configuration and must fail before a success report can be emitted.

The normal Windows/Linux CTest path runs a deterministic 50,000-operation smoke using the same mutation and verification algorithm. A successful smoke report must still contain:

- `mode: "smoke"`;
- `certification_minimum_operations: 10000000`;
- `certification_eligible: false`.

A smoke PASS is implementation coverage only. It is never mutation certification evidence.

## Integrity checks

At deterministic checkpoints and at the end of the run, the harness recomputes:

- record-count aggregate;
- total text-byte aggregate;
- total layout-height aggregate;
- search-summary aggregate;
- exact record rank for every live record;
- exact text-offset prefix for every live record;
- exact layout-height prefix for every live record;
- exact record identity including `source_record_index`;
- sampled prefix aggregates at zero, midpoint and full length;
- deterministic digest over the complete logical order and every record field.

The live sequence digest must equal the independently computed oracle digest.

## Snapshot authority

At each checkpoint the harness retains a copy-on-write snapshot plus its contemporaneous oracle state. At the next checkpoint that old snapshot is revalidated after intervening mutations.

This prevents a live tree that appears correct while silently mutating old snapshots from passing the long-run authority.

## Raw receipt

A passing report records at minimum:

- exact schema and authority;
- mode;
- requested and completed operation counts;
- certification minimum and eligibility;
- deterministic RNG seed;
- count of each of the five mutation classes;
- number of integrity checkpoints;
- live and oracle logical-order digests;
- final aggregate values;
- zero logical-order mismatch count;
- zero integrity mismatch count;
- terminal gate result.

For final certification, all five operation counts must be nonzero and their sum must equal `operations_completed`.

## Failure semantics

Configuration failure, a mutation API failure, old-value mismatch, erase-identity mismatch, aggregate drift, snapshot drift, prefix drift, logical-order drift or digest mismatch fails closed.

A code/integrity failure is not an excuse to rerun the identical candidate/seed until it happens to pass. A fixed candidate creates new evidence.

## Final M8 boundary

This authority covers the mixed-mutation axis only. It cannot compensate for missing Titan, device-profile, 24-hour soak, storage-crash, fuzzing or final binder evidence.
