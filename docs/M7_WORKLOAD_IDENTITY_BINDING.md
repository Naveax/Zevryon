# M7 workload identity binding

## Purpose

M7 schema version 2 binds every benchmark run to an exact canonical workload in addition to the corpus hash. This prevents cross-engine comparisons where the same corpus is used with different viewport sizes, search modes, mutation counts, scroll distances, or other operation parameters.

## Canonical workload hash

`canonical_workload_sha256()` serializes the workload object as UTF-8 JSON with sorted object keys and compact separators, then computes SHA-256. Object key ordering therefore cannot change workload identity. Array ordering remains significant because operation order is part of the benchmark.

The workload object should contain every operation and parameter that can change measured behavior, including viewport dimensions, open mode, scroll distance and cadence, search query/mode, mutation batch definition, and copy/export operation definition.

## Schema v2

Each raw run adds:

- `workload_sha256`

An engine aggregate is rejected if its successful or failed raw runs mix workload identities. Each aggregate publishes the workload SHA-256 alongside its existing engine, corpus, system-state, and metric evidence.

The leadership evaluator additionally requires every measured engine to have the same workload identity. If measured engines differ, `leadership_claim_allowed` is false and `workload_identity_mismatch` is emitted as a blocker.

## Backward compatibility

Schema version 1 remains readable as historical evidence. `scripts/evaluate_m7_competitor_lab.py` dispatches version 1 payloads to the original evaluator and version 2 payloads to the workload-bound evaluator. New M7 campaigns should use schema version 2.

## Tests

`m7-competitor-lab-workload-identity-smoke` verifies:

- canonical hashing is independent of JSON object-key order;
- workload identity survives raw-run aggregation;
- a cross-engine workload mismatch blocks leadership;
- mixed workload identities inside one engine aggregate are rejected;
- missing `workload_sha256` fails closed.
