# Zenith source-record semantic adoption

## Scope

This slice is the production adoption half of issue #157. It wires the real
`ZenithTabRuntime` to the disk-backed logical-node authority through immutable
physical `source_record_index` identity.

The runtime path is:

```text
Layout/source physical identity
    -> source_record_index
    -> LogicalNodeRecordIndexAuthoritativeReader
    -> bounded posting window
    -> same-open store-bound arena
    -> node/tag/role/style/attribute semantics
```

`LayoutFragment.logical_id` is not a logical-node id and must never be used as
one.

## UI-lane rule

`semantic_nodes_for_source_record_on_lane()` rejects `FrameExecutionLane::Ui`
before constructing or opening the record index or logical-node arena. The
compatibility `semantic_nodes_for_source_record()` entry point is worker-lane
authority.

The normal `ZenithTabRuntime::open()` path does not require semantic sidecars to
exist. A document that has not admitted the semantic index can still use the
existing layout runtime; only a worker semantic query requires the index.

## Same-handle authority

Production semantic materialization must not validate a posting against one
arena and then reopen the arena path to fetch browser semantics.

The adopted record-index reader therefore exposes bounded node, attribute and
semantic-resolution primitives from the exact `LogicalNodeArenaV2StoreBoundReader`
instance already used for posting overlap validation.

The adopted authoritative reader also obtains first/last/count head metadata
from the exact `heads.bin` handle already held by its low-level storage reader.
It does not reopen `heads.bin` after identity establishment.

This keeps the production authority chain on one already-validated handle set:

```text
head snapshot -> posting chain/overlap -> arena node -> interned semantics
```

The create-only sidecars remain immutable after publication. The same-handle
contract additionally removes path-replacement TOCTOU between these authority
steps.

## Boundedness

A source-record semantic query is bounded independently by:

1. record-index posting count (`max_nodes`, with the hard record-index ceiling);
2. runtime semantic-node count (`ZenithSemanticNodeWindowConfig::maximum_nodes`);
3. per-node and total attribute budgets;
4. total resolved semantic bytes.

The effective node count is the minimum of the caller request and the runtime
semantic-node budget.

If total attribute or semantic-byte budget is exhausted after one or more
complete nodes, the result truncates before the first unmaterialized posting and
returns that exact posting ordinal as the continuation cursor. No partial node
is returned.

If one node by itself exceeds a configured per-node or semantic-byte bound, the
query fails closed rather than returning incomplete browser semantics.

`ZenithRecordSemanticWindowResult` reports the semantic bytes and attribute
count actually materialized in that result window.

## Semantic payload

Each returned `ZenithRecordSemanticNode` carries both:

- the authoritative record-local overlap posting (`record_byte_offset` and
  `record_byte_length`); and
- the complete bounded `ZenithSemanticNode` payload from the same store-bound
  arena, including topology/source fields, tag, role, style and out-of-line
  attributes.

## Tests

Focused runtime tests build a real native store, compact layout arena,
store-bound logical-node arena and record index, then open a real
`ZenithTabRuntime`.

They cover:

- worker-lane physical-record lookup;
- cross-record logical-node continuation;
- tag/role/style materialization;
- out-of-line attribute name/value/flags materialization;
- exact semantic-byte and attribute accounting;
- semantic-byte truncation returning the first deferred posting cursor;
- continuation from that cursor;
- UI-lane rejection while the record index is intentionally absent;
- worker failure when the authoritative index is absent.

The existing record-index authority/corruption tests continue to run against the
adopted single-handle implementation.

## Admission boundary

This document describes candidate behavior until the exact adoption head passes
the full repository Windows/Linux CI and is merged.

Issue #157 and the remaining M1 browser logical-node checklist items must not be
closed merely because this file exists. They become eligible only after the
record-index infrastructure and this real `ZenithTabRuntime` production path are
both admitted on `main`.
