# M5 — Shared Record-Length Authority

## Purpose

This slice introduces the bounded process-wide metadata authority needed to make velocity prefetch record-length-aware without adding blocking descriptor I/O to the UI thread and without retaining a metadata index per tab.

`SharedRecordLengthAuthority` caches `(store root, physical record index) -> record length` in a thread-safe LRU. The default capacity is 4096 metadata entries. That is a cache budget, not a browser tab/session limit: the tab registry remains policy-unbounded and cache eviction only removes old metadata.

Each normalized store-root key is independently bounded to 1024 bytes by default. The authority therefore cannot grow merely because more tabs exist. Resolver work runs outside the authority mutex so a metadata miss does not block unrelated cache hits behind disk I/O.

## Learned EOF

`remember()` lets the worker publish a record length learned from an exact short EOF result. A later tab/session requesting the same physical record can consume that process-shared metadata without repeating the discovery read.

## Record-bound prefetch clamp

`clamp_prefetch_to_record()` is the pure policy layer that combines:

- scroll direction;
- the visible source edge;
- the velocity planner's predicted source offset;
- requested window bytes;
- authoritative record length.

Forward behavior:

1. keep the predicted request unchanged when it fits;
2. when prediction crosses EOF but a complete window still exists after the visible edge, use the last complete window;
3. otherwise issue the exact remaining tail from the visible edge;
4. if the visible edge is already EOF, issue nothing.

Reverse behavior keeps the request before the visible edge and narrows prefix reads so speculative I/O does not unnecessarily cross back into already-visible bytes.

## Resource invariants

This slice does not add per-tab worker threads, per-tab metadata caches, unbounded ready payloads, or UI-thread disk access. The authority is intended to be owned by the process-level shared prefetch path and queried by worker-side resolvers in the next integration slice.

## Focused validation

The focused C++20 test covers shared cache hits, bounded LRU eviction, learned-EOF reuse, store-root identity separation, resolver failure, forward full-window clamp, exact forward tail, EOF suppression, reverse exact prefix, and invalid directional prediction.
