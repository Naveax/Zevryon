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

Cache accounting includes vector capacity and conservative container overhead. Eviction occurs before insertion and resident-byte/peak-byte metrics are reported.

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

## Scope boundary

These measurements cover disk-backed indexing, viewport selection, bounded UTF-8 source access, scroll anchoring, height correction, and deterministic average-advance fragment production. They do not yet include real font shaping, bidi, grapheme segmentation, CSS inline formatting, paint, or compositor work.
