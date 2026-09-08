# Zenith semantic-node runtime consumer

## Purpose

The record-level hot-scroll/layout path cannot be reinterpreted as browser-node semantics: its `logical_id` identifies logical records, not logical browser nodes. Forcing those identifiers into logical-node arena lookup would merge two different identity domains and corrupt source/layout semantics.

This slice therefore adds a separate production browser-runtime consumer for the authoritative physical-store-bound logical-node arena v2:

- `ZenithSemanticNodeWindow` opens `LogicalNodeArenaV2StoreBoundReader` and performs bounded ordinal-window materialization;
- `ZenithSemanticRuntimeConsumer` supplies execution-lane authority, lazy reopen, serialization and telemetry around that window.

The implementation is part of `zevryon-massivedoc-core`; it is not a benchmark-only or test-only adapter. Runtime semantic reads therefore inherit the arena-v2 requirement that payload SHA-256, stable physical record sequence and exact record count still match the current native store before any browser-node semantics are exposed.

## Bounded materialization

One semantic window is constrained by all of the following independent limits:

- maximum nodes;
- maximum attributes per node;
- maximum total attributes;
- maximum resolved semantic bytes across tags, roles, styles, attribute names and attribute values.

Configuration values themselves have hard ceilings. This prevents a caller from turning a nominally bounded API into an unbounded reserve request by supplying arbitrarily large limits.

A result is returned only at complete-node boundaries. If adding the next node would exceed a cumulative window budget, the reader returns the nodes already completed, sets `truncated=true`, and leaves `next_ordinal` pointing at the first node not returned.

The same complete-node rule applies when the next node exceeds the configured total-attribute or semantic-byte budget by itself: if the current window already contains one or more complete nodes, those nodes are returned and continuation stops before the oversized node. A read that starts at that oversized node fails closed. Per-node attribute-limit violations always fail closed.

The window exposes the frozen arena record unchanged alongside resolved semantic values, preserving:

- stable logical node id;
- physical source identity/range;
- parent/first-child/next-sibling topology;
- node flags;
- complete attribute flags and values.

## Execution-lane authority

Logical-node arena reads may perform filesystem I/O. `ZenithSemanticRuntimeConsumer` therefore enforces the same basic browser-runtime rule used by blocking layout paths:

- `FrameExecutionLane::Ui` is rejected before the arena/store binding is opened;
- `FrameExecutionLane::Worker` may lazily open and query the store-bound v2 arena.

The convenience `read()` method is explicitly worker-lane authority.

The reader is lazy so ordinary Zenith stores without a logical-node v2 sidecar continue to open through their existing record/layout paths. A semantic request against a store with no authoritative bound arena fails closed at this consumer boundary instead of breaking unrelated runtime startup.

## Concurrency

One runtime consumer serializes access to its disk-backed reader with a mutex. The same lock protects lazy initialization and semantic-consumer telemetry, avoiding concurrent operations on the underlying positional-reader state.

## Telemetry

The consumer records saturating counters for:

- requests;
- successful and failed windows;
- UI-lane rejections;
- truncated windows;
- materialized node count;
- materialized attribute count;
- resolved semantic bytes.

Telemetry is diagnostic and never defines semantic correctness.

## Identity boundary

This consumer deliberately does not modify `LayoutFragment::logical_id`. Record-level document order/layout remains the existing record authority. Browser-node topology and interned semantic identities remain the store-bound logical-node arena-v2 authority.

That separation is mandatory until a later architecture explicitly versions and joins those identity domains.

## Tests

Focused tests use a real native store plus `LogicalNodeArenaV2StoreBoundWriter` and cover:

- exact tag/role/style and attribute reconstruction after authoritative arena-v2 reopen;
- parent/sibling topology preservation;
- node-count continuation windows;
- cumulative attribute-budget truncation at a complete-node boundary;
- semantic-byte truncation at a complete-node boundary;
- continuation then single-node over-budget failure;
- hard configuration-ceiling rejection;
- UI-lane rejection followed by successful worker-lane lazy open;
- missing bound-arena and invalid-config failure telemetry.

This slice strengthens the production consumer side of the M1/M2 logical-node architecture. It does not make the record hot-scroll identifiers equivalent to browser-node identifiers and does not claim the later 67,108,864-node certification envelope.
