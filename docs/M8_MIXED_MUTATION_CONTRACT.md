# M8 mixed-mutation integrity contract

## Authority

Machine-readable report schema: `zevryon.m8.mixed-mutation.v2`.

Authority: `m8-sequence-mixed-mutation-integrity-v2`.

The target under test is the production `ChunkedOrderStatisticsSequence` mutation surface. A deterministic `std::vector<SequenceRecord>` oracle is maintained independently and compared against the live sequence throughout the run.

## Required mutation classes

The harness mixes all five production mutation classes:

- insert;
- erase;
- move;
- height update;
- search-summary update.

The workset stays intentionally bounded while the operation count grows. M8 is testing long-running mutation correctness, copy-on-write snapshot survival and sequence invariants, not rewarding a harness for exhausting the runner with an ever-growing reference container.

A passing smoke or certification report requires every mutation class count to be nonzero and requires the five counts to sum exactly to `operations_completed`.

## Certification threshold

Final mutation certification requires **at least 10,000,000 completed mixed mutations** in one admitted run.

`--certification` with fewer than `10,000,000` requested operations is invalid configuration and must fail before an evidence report can claim eligibility.

The normal Windows/Linux CTest path runs a deterministic 50,000-operation smoke using the same mutation and verification algorithm. A successful smoke report must still contain:

- `mode: "smoke"`;
- `certification_minimum_operations: 10000000`;
- `certification_threshold_met: false`;
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

## Terminal raw receipt

A report is emitted for every structurally valid run that reaches the mutation harness, whether integrity passes or fails. A terminal report records at minimum:

- exact schema and authority;
- mode;
- requested operation count;
- the exact number of successfully completed mutations at termination;
- certification minimum, threshold receipt and eligibility;
- deterministic RNG seed;
- count of each of the five mutation classes;
- count-sum and all-class coverage receipts;
- number of integrity checkpoints;
- live and oracle logical-order digests when available;
- final aggregate values;
- logical-order mismatch count;
- integrity mismatch count;
- explicit failure reason on a failing run;
- terminal gate result.

A failure after operation N must not be serialized as though all requested operations completed. `operations_completed` is advanced only after the live mutation and corresponding oracle mutation both complete successfully.

Invalid command configuration, such as requesting certification below the frozen minimum, is rejected before a run starts and is not raw mutation evidence.

## Failure semantics

A mutation API failure, old-value mismatch, erase-identity mismatch, aggregate drift, snapshot drift, prefix drift, logical-order drift, digest mismatch, missing mutation class or operation-count inconsistency fails closed with exit status `2` and a machine-readable terminal failure report.

Configuration/report-I/O failures use exit status `1`.

A code/integrity failure is not an excuse to rerun the identical candidate/seed until it happens to pass. A fixed candidate creates new evidence.

## Final M8 boundary

This authority implements the mixed-mutation evidence harness. Final M8 still requires an admitted certification-mode run with at least ten million completed operations.

This axis cannot compensate for missing Titan, device-profile, 24-hour soak, storage-crash, fuzzing or final binder evidence.
