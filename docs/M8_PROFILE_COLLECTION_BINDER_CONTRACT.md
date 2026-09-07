# M8 four-profile collection binder contract

## Purpose

The existing no-compensation profile evaluator requires exactly four observations in one `zevryon.m8.profile-observations.v1` artifact. Physical certification, however, requires each named profile to come from a qualifying physical or separately attested environment. One process on one desktop cannot legitimately manufacture four physical device identities.

The collection architecture is therefore:

1. one **per-profile case** is collected on each qualifying environment;
2. each case is preserved as `zevryon.m8.profile-case.v1` with authority `m8-raw-profile-case-v1`;
3. `m8_profile_collection_binder.py` consumes exactly four cases plus one canonical certification-mode Titan report;
4. the binder recomputes every derived observation field from raw samples;
5. only then does it create the exact four-profile `zevryon.m8.profile-observations.v1` evaluator input and a `zevryon.m8.profile-collection.v1` collection receipt.

This resolves the physical-host boundary without relaxing the existing exact four-profile evaluator schema.

## Per-profile case schema

A case contains exactly:

- `schema`;
- `authority`;
- `case_id`;
- `candidate_commit`;
- `candidate_tree`;
- `device_class`;
- `titan`;
- `physical_host`;
- `runtime_policy`;
- `raw`.

Precomputed `score_100`, `gate_passed`, derived percentile summaries or other verdict fields are forbidden rather than ignored.

Case IDs must be unique in one collection. Device classes must be unique and the final set must be exactly:

- `legacy-phone`;
- `mid-phone`;
- `modern-phone`;
- `desktop`.

## Candidate and Titan identity

Every case must match the canonical Titan report on:

- candidate commit;
- candidate tree;
- Titan report SHA-256;
- Titan container SHA-256;
- Titan logical-payload SHA-256.

The Titan report must itself be:

- schema `zevryon.m8.titan-fixture.v1`;
- authority `m8-canonical-titan-fixture-v1`;
- `mode: certification`;
- threshold-met;
- certification-eligible;
- gate-passing.

Smoke Titan reports cannot be bound into physical profile certification.

## Physical-host requirements

Each case binds one physical-host receipt by SHA-256. The case is invalid unless:

- the host receipt authority is named;
- the host is explicitly qualified;
- the receipt device class equals the case device class;
- physical RAM meets the frozen profile minimum;
- the owned process group is complete;
- the memory authority is `aggregate-pss`.

RSS, working set, private bytes or another resident-memory counter cannot be relabelled as PSS.

## Runtime-policy requirements

Each case records the runtime profile and hot/warm/cold cache budgets and allocations.

The binder verifies:

- the runtime-policy profile equals the case device class;
- the policy reports `within_profile_budgets: true`;
- each budget equals the frozen `DEVICE_PROFILES` decimal-MB budget;
- allocated hot/warm/cold bytes do not exceed the matching budget.

Unused cache budget is allowed and does not relax latency limits.

## Raw recomputation

The binder derives the final observation rather than trusting a case summary.

### Memory

`process_group_pss_mb` is the maximum value in the nonempty raw PSS sample array.

### First viewport

The case records the raw streaming and preindexed first-viewport elapsed milliseconds. Their production measurement boundaries are defined by `M8_PROFILE_MEASUREMENT_CONTRACT.md`.

### Scroll

Exactly 257 measured scroll samples are required. The binder computes:

- P99 using linear interpolation at position `(N - 1) * 0.99` in sorted order;
- maximum normal stall as the maximum of the same raw sample set.

### Exact search

Cold and warm exact-search milliseconds are taken from their separately preserved raw timings. The collector must also preserve their production hit/cache receipts; final collector admission validates those phase receipts.

### Mutation

Exactly 257 measured mutation samples are required. P95 is linearly interpolated at `(N - 1) * 0.95`.

### Copy throughput

The binder computes:

`copy_throughput_mib_s = (source_bytes / 1,048,576) / elapsed_seconds`.

A copy is treated as a data-loss event when any of these disagree with the Titan authority:

- selected/source bytes;
- output bytes;
- output SHA-256;
- cancellation state.

Invalid UTF-8 output increments the invalid-UTF8 axis. Abnormal termination or a nonzero probe return code increments the crash/OOM axis.

These derived failures are added to any explicitly preserved correctness-event counts. A corrupt copy therefore cannot remain `data_loss_events: 0` merely because a summary field said so.

## Output

The verdict-free observations output is exact schema `zevryon.m8.profile-observations.v1` and contains:

- candidate commit;
- candidate tree;
- four derived observations.

The collection receipt is schema `zevryon.m8.profile-collection.v1`, authority `m8-four-profile-collection-binder-v1`, and binds:

- candidate commit/tree;
- Titan report/container/payload hashes;
- all four case IDs and case SHA-256 values;
- physical-host receipt hashes;
- every derived observation;
- SHA-256 of the exact observations output;
- the recomputed four-profile no-compensation gate result.

The binder calls the admitted profile observation evaluator on its own generated document. It does not implement a second scoring rule.

## Exit semantics

- exit `0`: evidence is valid and the recomputed four-profile gate passes;
- exit `2`: evidence is valid but at least one recomputed profile check fails;
- exit `1`: evidence structure, identity, qualification, Titan binding or runtime-policy authority is invalid.

A valid exit-2 result is preserved evidence. It is not a harness failure and may not be silently discarded in favor of a preferred repeat.

## Remaining collector boundary

This binder does not itself run the production browser/document engine. The still-open per-profile collector must produce each raw case from the exact measurement boundaries in `M8_PROFILE_MEASUREMENT_CONTRACT.md`, including phase receipts and real aggregate PSS samples.

Final M8 bundle admission must require both the four-profile observations artifact and this collection receipt, then re-hash/revalidate the four per-profile case artifacts rather than trusting the binder's summary boolean.
