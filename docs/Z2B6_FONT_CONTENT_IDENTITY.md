# Z2B-6 — Deterministic Font Content Identity

## Purpose

Z2B-6 removes caller-defined cache identity from the normal verified-font path. `compute_font_content_identity` derives a portable key from the exact font bytes and selected face index, and `VerifiedFontResourceCache::get_or_build_content_addressed` uses that identity directly.

## Hash construction

The identity input is framed in this exact order:

1. the 24-byte domain marker `zevryon.font-content.v1\0`;
2. the selected face index as one big-endian 32-bit integer;
3. the exact byte length as one big-endian 64-bit integer;
4. the exact font bytes.

The framed message is hashed with SHA-256. The first 128 digest bits become `(high, low)`, while the face index remains explicit in `FontContentIdentity` and `VerifiedFontResourceCacheKey`.

The cryptographically negligible all-zero 128-bit prefix is deterministically remapped to `low = 1` to satisfy the cache's non-zero identity invariant.

## SHA-256 implementation

The core is:

- dependency-free;
- allocation-free;
- endian-independent;
- streaming;
- bounded by the SHA-256 64-bit bit-length field;
- reusable after explicit `reset`;
- closed after `finish` until reset.

No platform crypto API is required, so Linux, macOS, and Windows produce identical bytes.

## Cache integration

`get_or_build_content_addressed`:

1. computes the domain-separated identity;
2. optionally publishes that identity to the caller;
3. converts it to `VerifiedFontResourceCacheKey`;
4. enters the existing single-flight, collision-guarded, bounded cache path.

The existing explicit-key API remains available for loaders that already possess an independently authenticated content identity.

## Collision defense

The 128-bit key selects a cache candidate, but resident hits with supplied source bytes still run the Z2B-5 byte-for-byte collision guard. A digest collision therefore cannot silently substitute different bytes.

## Certification

The focused suite covers:

- SHA-256 empty-message vector;
- SHA-256 `abc` vector;
- multi-block standard vector;
- one-byte chunking;
- irregular chunking;
- one million `a` vector;
- finalize/reuse/reset state rules;
- pinned domain-separated font identity;
- face-index separation;
- payload-length and payload mutation separation;
- cache-key conversion;
- content-addressed cache build and identical-handle hit;
- strict GCC, AppleClang, and MSVC builds;
- Linux and macOS ASan/UBSan;
- three independent 16 MiB throughput distributions.

## Permanent performance gates

For one 16 MiB identity calculation on the hosted Ubuntu runner:

- P50 throughput must be at least 100 MiB/s;
- P95 must be at most 160 ms;
- P99 must be at most 200 ms;
- maximum must be at most 300 ms.

These limits include domain framing and complete SHA-256 processing, but no file I/O or resource construction.

## Explicit boundary

This slice does not load files, use file freshness metadata, persist digests, authenticate external identities, memory-map fonts, cache HarfBuzz objects, parse semantic OpenType tables, rasterize, or paint.
