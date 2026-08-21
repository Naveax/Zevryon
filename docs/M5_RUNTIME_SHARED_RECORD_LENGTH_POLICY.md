# M5 — Runtime Shared Record-Length Policy

## Purpose

This slice wires the bounded process-shared record-length authority into `ZenithTabRuntime` without introducing UI-thread descriptor I/O.

Each runtime may receive a non-owning `SharedRecordLengthAuthority*` in `ZenithTabRuntimeConfig`. The device-profile factory accepts the same pointer, allowing the browser/process owner to construct one authority and share it across an unbounded number of tab runtimes. The authority must outlive those runtimes.

## Cache-only hot path

Before a speculative request is queued, the runtime performs only `try_get()` against the in-memory authority. A metadata miss does not invoke a resolver and does not touch disk. On a miss, the existing velocity-prefetch request remains unchanged.

On a metadata hit, the runtime applies the record-bound policy before queue admission:

- a prediction that crosses EOF is moved to the last valid full window when possible;
- a shorter remaining region becomes an exact tail request;
- a visible edge already at EOF suppresses speculative work entirely;
- reverse prefix requests are narrowed so they do not read unnecessarily across the visible edge.

## Learning EOF without extra I/O

The default store slice semantics already return fewer bytes only when the requested range reaches record EOF. When a successful prefetch returns `0 < actual_bytes < requested_bytes`, the runtime derives:

`record_length = byte_offset + actual_bytes`

and publishes it to the shared authority before exact-tail cache canonicalization. A later tab/session viewing the same store root and physical record can therefore clamp its request with no additional descriptor read.

A full-sized result never infers EOF. Empty or failed results never teach metadata.

## Telemetry

`ZenithTabRuntimeStats` now exposes:

- `record_length_cache_hits`;
- `record_length_clamps`;
- `record_length_eof_suppressions`;
- `record_length_learns`;
- `record_length_learn_failures`.

These counters separate metadata effectiveness from ordinary prefetch/cache admission statistics.

## Resource invariants

This integration does not add a tab ceiling, per-tab metadata cache, per-tab thread, or UI-thread disk operation. Cache capacity remains process-wide and bounded independently of tab count.

## Remaining worker-side step

A cold authority miss still allows the original speculative request to run once. The next worker-side slice can resolve record length before payload I/O, but that requires the shared executor/result contract to publish a canonicalized request key rather than mutating bytes behind a `const request`. Until that contract changes, the learned-EOF path is correctness-preserving and avoids key/data mismatches.
