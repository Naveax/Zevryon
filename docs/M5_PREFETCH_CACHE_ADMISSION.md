# M5 — Exact Prefetch Cache Admission

## Purpose

This slice completes the first useful handoff between process-level speculative source prefetch and the authoritative hot-scroll source-window cache.

Previously a successful shared prefetch warmed the operating-system file cache, then its returned payload was discarded by `ZenithTabRuntime`. That could reduce physical storage latency, but the next hot-scroll query still had to issue its own `StoreReader::read_record_slice()` call.

The new path admits an exact successful prefetch result directly into the existing hot-scroll LRU under the identical physical `SourceWindowKey` used by synchronous reads.

## Exact-key contract

`ZenithHotScrollSession::admit_prefetched_source_window()` accepts:

- immutable `source_record_index`;
- exact source byte offset;
- exact requested byte count;
- the returned byte vector.

Admission fails closed unless:

- the session is already open;
- request length is in `(0, kIoWindowBytes]`;
- returned byte count is exactly equal to the requested byte count;
- the existing source-window byte budget can admit the conservative cache charge after normal LRU eviction.

A short EOF result, partial read, oversized request, allocation failure, or over-budget result is never inserted as if it were a complete synchronous cache entry. Such a speculative read may still have warmed OS page cache, but correctness continues through the normal synchronous fallback.

## Memory behavior

Prefetched bytes use the existing source-window LRU and its existing `max_source_window_cache_bytes` limit. There is no second tab-local payload cache and no new unbounded retention path.

If the exact key already exists, admission simply refreshes its LRU position. Otherwise normal source-cache eviction runs before insertion and current/peak cache-byte accounting is updated through the same resident-byte contract.

Background and critical tab-pressure trimming release these admitted windows exactly like synchronously populated source windows.

## Runtime handoff

When `ZenithTabRuntime` drains a successful ready result from `SharedSourcePrefetchPool`, it moves the payload into `admit_prefetched_source_window()` using the request's physical source identity and exact offset/size.

Telemetry distinguishes:

- successful ready-result drains;
- failed ready-result drains;
- exact hot-cache admissions;
- hot-cache admission rejections.

The shared pool relinquishes its global ready-result charge when the result is taken; successful admission then transfers ownership into the already-bounded tab-local hot-scroll source LRU. The same payload is not retained in both places.

## Certification test

The integration test now performs a direct exact-window proof:

1. clear the hot-scroll source cache;
2. read one exact 64 KiB physical window from the immutable store;
3. verify a one-byte-short payload is rejected;
4. admit the exact physical window;
5. execute a top-of-document checkpoint-accelerated layout whose first required source key is the same window;
6. require `source_bytes_read == 0` and at least one source-window cache hit with zero source-window cache misses.

This proves the handoff can remove the synchronous `StoreReader` call rather than merely making that call faster through OS caching.

The tab-runtime integration test also uses real store bytes for its gated executor, drains the successful result, and requires at least one hot-cache admission before critical hidden pressure releases the cache.

## Boundary

This slice does not yet implement multi-window velocity prediction, cross-record lookahead, automatic adaptive prefetch distance, or measured prefetch hit-rate policy. Those should be layered on top of exact-key correctness and must remain subordinate to frame budget, hidden-tab suppression, global shared-pool bounds, and memory pressure.
