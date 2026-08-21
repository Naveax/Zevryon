# ZENITH Hot-Scroll Session

## Purpose

ZENITH Hot-Scroll extends persistent sparse layout checkpoints with a stateful, byte-bounded session for repeated and adjacent viewport queries. The session removes repeated store, arena, and checkpoint-open work without retaining the full logical source or a full DOM in memory.

## Owned state

A session keeps these objects open for its lifetime:

- one `StoreReader`;
- one `CompactArenaReader` with the Fenwick height directory;
- a byte-budgeted LRU of parsed `LayoutCheckpointIndex` objects;
- a byte-budgeted LRU of source windows;
- reusable source and fragment scratch buffers.

The session is intentionally not thread-safe. A document/render worker owns one session or serializes access externally.

## Default budgets

| Resource | Default budget |
|---|---:|
| Parsed checkpoint cache | 1,000,000 bytes |
| Source-window cache | 512 KiB |
| Sparse checkpoint stride | 16 KiB |
| Physical source read window | 64 KiB |

Cache accounting includes vector capacity and conservative container overhead. Eviction occurs before insertion and resident-byte/peak-byte metrics are reported. Source and fragment scratch capacities are also exposed in telemetry so hidden per-tab working-set retention cannot disappear behind cache-only accounting.

## Multi-tab memory pressure

`ZenithHotScrollSession::trim_memory()` provides two explicit pressure levels for later tab/process scheduling without changing document authority:

1. **Background** releases the complete source-window LRU, its hash/list container storage, source scratch capacity, and fragment scratch capacity. Parsed checkpoint metadata remains resident so returning to a background tab can resume with checkpoint hits and only bounded source-window I/O.
2. **Critical** performs the background trim and additionally releases the parsed-checkpoint LRU plus its container storage. Immutable `StoreReader`, `CompactArenaReader`, document order, and height authority remain open, so the page is not reloaded or discarded.

The older `clear_source_window_cache()` entry point now uses the same full source-working-set release path rather than merely erasing entries while retaining container buckets and scratch capacities.

Telemetry records background/critical trim counts, charged bytes reclaimed, current/peak source scratch capacity, and current/peak fragment scratch capacity. `trim_reclaimed_bytes` is deliberately conservative: it counts charged cache bytes plus observable vector capacities and does not pretend to measure allocator metadata that the process cannot attribute exactly.

This is a lower-layer primitive for future Live100/tab scheduling. It does **not** claim Z11 completion, automatic tab suspension, timer throttling, network suspension, or renderer-process lifecycle policy.

## Correctness and fallback

A persistent checkpoint is accepted only when record identity, source length, layout configuration, file size, entry monotonicity, and checksum all match. Missing, stale, corrupt, or disabled checkpoints return the safe fallback signal instead of mutating payload data.

The canonical source store remains immutable. Height correction changes only the rebuildable arena metadata. Full CRC32 and SHA-256 verification runs after the certification benchmark.

## Query modes

The `zevryon-zenith-hot` executable measures two in-process profiles:

1. **Random hot-scroll:** deterministic positions around a giant-record center. Parsed checkpoints stay warm, while source windows begin cold.
2. **Adjacent hot-scroll:** one-line scroll increments. Both checkpoint and source-window caches stay warm.

Each profile reports P50/P95/P99/maximum latency, physical source bytes read, zero-I/O query count, cache hits, misses, evictions, resident bytes, and peak resident bytes.

## Certification gates

The 64 MiB giant-record CI certification requires:

- 257 deterministic random queries and 257 adjacent queries;
- random P95 at or below 2.0 ms;
- adjacent P95 at or below 0.5 ms;
- at most one 64 KiB physical source read per query;
- at least 95% zero-source-I/O adjacent queries;
- no warmed checkpoint reparse;
- sparse index overhead below 0.2% of source bytes;
- checkpoint and source-window caches below their configured byte budgets;
- complete post-run payload verification.

The unit regression additionally verifies that a background trim preserves checkpoint reuse while forcing bounded source reload, and that a critical trim releases both cache classes while the same session remains correct and usable.

## Scope boundary

These measurements cover disk-backed indexing, viewport selection, bounded UTF-8 source access, scroll anchoring, height correction, deterministic average-advance fragment production, and tab-pressure working-set release. They do not yet include real font shaping, bidi, grapheme segmentation, CSS inline formatting, paint, compositor work, JavaScript timers, network activity, or process suspension.
