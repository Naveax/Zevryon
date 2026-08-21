# M5 — Shared Prefetch Executor V2

## Purpose

The shared prefetch pool now has an explicit V2 execution contract that returns both the bytes and the canonical request that actually produced those bytes. This closes the correctness gap where a worker could clamp an offset/length internally but the pool would still publish the original cache key.

## Identity rules

A V2 executor may narrow only speculative location/size. The pool rejects a result if the executor changes the physical `record_index` or authoritative `PrefetchTicket`, expands `max_bytes` beyond the admitted request, or returns more bytes than the canonical request allows.

Legacy executors remain supported and publish the original request unchanged.

## Worker-side cold record-length resolve

When `SharedSourcePrefetchPoolConfig::record_length_authority` is supplied and a request carries `visible_edge_offset`, the built-in worker:

1. queries the process-shared bounded record-length authority;
2. on a cache miss, reads only the 8-byte record-length field from `records.idx` on the worker thread;
3. clamps the velocity prediction to a full valid window or exact tail before payload I/O;
4. suppresses the run entirely when the visible edge is already EOF;
5. publishes the canonical request with the bytes.

No descriptor I/O is introduced on the UI thread.

## Unbounded-tab resource invariant

The previous built-in pool retained a lazy `StoreReader` inside each session. That made native reader retention proportional to the number of tabs that had ever prefetched.

V2 removes the per-session reader. The built-in `StoreReader` now lives only for one worker execution, so concurrently live default readers are bounded by `worker_count` rather than session cardinality. The worker count remains independently bounded and does not grow with tabs.

The record-length authority remains a process-wide bounded LRU. Its entry count is a metadata resource budget, not a tab limit.

## Focused validation

The focused test uses a real MassiveDoc store and proves:

- a cold record-length miss canonicalizes an over-predicted request to the exact EOF tail;
- the returned cache identity is the canonical offset/size, not the original prediction;
- a second session reuses shared record metadata and suppresses work at EOF;
- a V2 custom executor may narrow offset/size;
- a V2 custom executor cannot rewrite physical record identity.

Local isolated compilation of the V2 pool contract was also run with C++20 and strict warning-as-error flags before publication. Full repository Windows/Linux CI remains separate evidence.
