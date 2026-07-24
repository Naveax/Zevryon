# Z2B-5 — Bounded Verified Font Resource Cache

## Purpose

`VerifiedFontResourceCache` is the bounded ownership layer between platform font loading and immutable verified font resources. It prevents repeated byte copies, SFNT/TTC parsing, table checksum scans, padding checks, and whole-font verification when the same keyed face is requested repeatedly.

## Key contract

A `VerifiedFontResourceCacheKey` contains two caller-defined 64-bit identity words and one face index. At least one identity word must be non-zero.

The key is intentionally not computed inside the cache. The loader/discovery layer may derive it from a content digest, stable file identity plus freshness metadata, packaged resource identity, or another collision-resistant source.

Two access forms are provided:

- `lookup`: key-only resident lookup; source bytes are not read or loaded.
- `get_or_build`: returns a resident resource or builds one from caller-owned bytes on a miss.

When source bytes are supplied for an existing key, they are compared byte-for-byte with the retained immutable resource. A mismatch is a fail-closed `KeyCollision`; the resident entry is not replaced.

## Single-flight publication

Concurrent misses for one key are serialized through a bounded in-flight table:

1. one caller becomes the builder;
2. other callers wait without copying or validating the font;
3. the builder creates one `VerifiedFontResource` outside the cache mutex;
4. publication occurs atomically under the cache mutex;
5. waiting readers receive the same `shared_ptr<const VerifiedFontResource>`.

No partially built resource is visible.

## Resource accounting

Three distinct ownership classes are preserved:

- `FontResourceBytes`: bytes physically owned by each immutable resource;
- `FontResourceCacheMetadata`: PMR storage for resident entries and in-flight keys;
- `FontResourceCacheRetention`: the sum of resources currently owned by the cache.

The retention class represents cache residency, not total process-lifetime bytes. After eviction or `clear`, an external reader may keep a resource alive through its own `shared_ptr`; that resource is no longer charged to cache retention but remains charged to its own `FontResourceBytes` ledger.

## Hard bounds

The cache is constructed with:

- a resident-byte hard limit;
- a metadata-byte hard limit;
- a maximum resident entry count.

A source larger than the retention hard limit fails before resource construction. Metadata allocation failures are reported separately and are visible through rejected ledger reservations.

## Eviction

Eviction is deterministic for a fixed operation sequence:

1. lowest last-use epoch is evicted first;
2. equal epochs are ordered by `(key.high, key.low, face_index)`.

The cache uses a max-entry-bounded linear table instead of an unordered container, avoiding rehash spikes and implementation-dependent bucket behavior.

## Failure atomicity

The following failures publish no output and do not replace existing entries:

- invalid key or output;
- key-only miss;
- source above retention limit;
- key collision;
- metadata budget rejection;
- allocation failure;
- SFNT/TTC parse failure;
- integrity verification failure.

## Certified behavior

The focused test suite covers:

- miss, population, and identical-handle hit;
- byte-for-byte collision rejection;
- exact resident-byte accounting;
- deterministic LRU eviction;
- external reader survival after eviction and clear;
- same-key concurrent single-flight publication;
- metadata and retention hard-limit rejection;
- invalid argument behavior;
- strict GCC, AppleClang, and MSVC builds;
- Linux and macOS ASan/UBSan execution.

## Explicit boundary

This slice does not implement file loading, content-digest generation, persistent disk cache, cross-process sharing, memory mapping, HarfBuzz object caching, semantic OpenType parsing, rasterization, or painting.
