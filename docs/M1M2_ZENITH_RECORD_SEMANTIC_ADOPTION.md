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

## Canonical admission

The production path is admitted on canonical `main`.

- record-index infrastructure: PR #158, exact head
  `4bee237c2263b139b10a29c03dd87956b90e2d5a`, exact-head CI run
  `34237003555` SUCCESS, merged as
  `04990b0ff89c59063e761e147afa7069d06d4d9d`;
- real `ZenithTabRuntime` semantic adoption: PR #159, exact head
  `31ed41801fca2a6b092e57274ee1f14d2deb10ee`, natural exact-head CI run
  `34240329146` SUCCESS with Windows/Linux full suites, Win32/i386 gates,
  both Unicode authority jobs and the Apple removal guard green, merged as
  `add6d43b925dc94e85111b4a57007673f142ca79`.

Issue #157 was closed only after the merge was verified on canonical `main`.
The 67,108,864-node certification envelope remains a separate M8 physical/raw
evidence boundary and is not fabricated by this admission.
