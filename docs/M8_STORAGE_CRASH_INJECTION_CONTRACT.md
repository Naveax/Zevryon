# M8 storage crash-injection contract

## Scope

M8 requires crash injection during every storage transaction stage without weakening the existing MassiveDoc correctness authority. This contract freezes the deterministic storage cut surface that a later process-level crash runner must exercise.

This slice does **not** claim that returning at a cut is equivalent to an operating-system crash, power loss, torn write, or storage-device failure. The cut surface exists so those destructive tests can target exact durable boundaries instead of guessing where to terminate the process.

## Publication transaction boundaries

The canonical `GenerationPublicationCut` matrix is:

1. `after_payload_flush`
   - immutable payload/index inputs have reached the durable payload-flush boundary;
   - no PREPARE record has been appended;
   - recovery must not discover a new generation protocol authority from this attempt.
2. `after_prepare`
   - the checksummed PREPARE journal record is durable;
   - no generation manifest has been published;
   - the previous committed generation remains authoritative.
3. `after_manifest_temp`
   - the new generation manifest exists only as a durable temporary file;
   - the final generation path has not been published;
   - restart must ignore the temp and a same-generation retry must be able to remove it and complete.
4. `after_manifest`
   - the final generation manifest has been durably published;
   - COMMIT has not been appended;
   - the published-but-uncommitted generation must never become authority;
   - an exact same-generation retry must durably move that abandoned final manifest into quarantine as `.uncommitted` evidence before republishing, rather than deleting it or treating it as committed authority.
5. `after_commit`
   - matching PREPARE and COMMIT records plus the final manifest are durable;
   - the normal post-write self-verification has not run;
   - restart recovery, not in-process success state, must prove the new generation is authoritative.

The pre-existing numeric values for `none`, `after_prepare`, and `after_manifest` are frozen. New values are appended rather than renumbering historical cut identifiers.

## Compaction transaction boundaries

The canonical `GenerationCompactionCut` matrix is:

1. `after_journal_temp`
   - compacted journal temp is durable;
   - the live journal remains unchanged.
2. `after_journal_replace`
   - the compacted live journal is durable;
   - stale manifests have not yet been quarantined.
3. `after_stale_quarantine`
   - one stale manifest has been durably moved into quarantine after the compacted journal became authoritative;
   - restart must retain the newest committed authority;
   - rerunning compaction must finish quarantining the remaining stale manifests without changing authority.

The pre-existing numeric values for `none`, `after_journal_temp`, and `after_journal_replace` are frozen.

## Deterministic restart gate

`m8-storage-crash-cut-tests` must run through the normal CTest path on Windows and Linux. It proves the following deterministic invariants:

- a cut before COMMIT cannot promote a new generation;
- durable PREPARE/temp/final pre-COMMIT states cannot permanently poison an exact same-generation retry;
- a published-but-uncommitted final manifest is preserved as quarantine evidence before retry rather than silently discarded;
- a cut after COMMIT is recoverable even though the normal post-write verifier never ran;
- a cut after one stale-manifest quarantine preserves the current authority and compaction is resumable;
- historical cut enum values do not silently change.

Any failure is fail-closed. A test cannot be converted to a warning or skipped merely because recovery still appears to work in a different cut.

## Required process-level follow-on

Final M8 crash certification still requires a destructive runner that, for every frozen cut above:

- starts from a known committed authority;
- reaches the requested cut in a child process;
- terminates the child without allowing normal transaction completion;
- starts a fresh recovery process;
- verifies exact source identity, authority manifest, segment inventory and logical order;
- records whether stale temp/journal/quarantine artifacts remain and whether a retry is safe;
- repeats enough times to expose timing-sensitive recovery defects;
- reports zero data corruption, invalid UTF-8 output, logical-order mismatch, crash-loop/OOM or authority regression.

That runner must publish machine-readable raw evidence. Deterministic CTest coverage is a prerequisite for it, not a substitute.

## Relationship to the rest of M8

Storage crash injection is only one M8 axis. Final 100/100 certification additionally requires, without compensation between axes:

- the full Titan adversarial content envelope, including giant-record, unbroken-token and pathological-grapheme dimensions;
- every device profile within its hard memory cap and its frozen latency/throughput contract;
- a continuous 24-hour soak;
- at least 10,000,000 mixed mutations;
- property fuzzing for Unicode, serializer, index and sequence authorities;
- zero crash/OOM, data corruption, invalid UTF-8 output or logical-order mismatch across the admitted evidence set.

No M8 completion claim is allowed until a final binder recomputes every required gate from raw evidence rather than trusting hand-authored summary booleans.
