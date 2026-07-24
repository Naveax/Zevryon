# Z2B-12A Prepared HarfBuzz Face Cache

## Purpose

Z2B-11A prepares one immutable `hb_blob_t`/`hb_face_t` pair and Z2B-11B uses it
on the catalog-facing shaping hot path. Callers could still prepare the same
catalog binding repeatedly or implement unbounded application-local maps.
Z2B-12A provides one bounded, synchronous, single-flight cache for immutable
prepared faces.

```text
CatalogFontFaceBinding
  -> stable generation/content cache key
  -> PreparedHarfBuzzFaceCache
       miss: one prepare_harfbuzz_face call
       hit: retained immutable shared handle
  -> prepared catalog shaping hot path
```

## Stable key

`CatalogFontFaceBinding` now retains the `FontContentIdentity` already computed
by the stable file loader. No byte re-read or repeat SHA-256 is performed.
The prepared-face cache key contains:

- generation ID;
- generation fingerprint;
- generation-local catalog face ID;
- content identity and selected SFNT/TTC face index.

A matching key backed by a different verified-resource object receives an
exact-byte collision guard before the resident prepared face is returned.
This protects callers using separate verified-resource cache instances and does
not trust resource IDs as globally unique.

## Bounds and accounting

The cache has three explicit construction limits:

- source-byte retention hard limit;
- metadata hard limit;
- maximum resident entry count.

Each resident face is conservatively charged by its complete verified source
byte length. This intentionally may over-count physical bytes already owned by
the verified-resource cache, but it places a strict upper bound on how much font
content the prepared cache may pin. The maximum entry count separately bounds
unmeasured native HarfBuzz blob/face overhead; the implementation does not
invent a native heap byte estimate.

Metadata and retention use the existing ledger-backed font-cache resource
classes. All failed admission, preparation, and publication paths leave output
empty and release reservations.

## Concurrency

Preparation occurs outside the cache mutex. The first miss installs one
in-flight key; concurrent misses for that exact key wait and then consume the
single published immutable face. Different keys may prepare concurrently.
`clear()` removes resident entries and retention charges but does not invalidate
prepared handles already held by callers.

## Eviction

Admission evicts least-recently-used entries until both the entry and retention
limits can accept the candidate. Equal-use ties use the complete semantic cache
key, giving deterministic victim selection independent of container order.

## Required certification

The real DejaVu Sans test proves:

- binding content identity equals the resolver identity;
- first miss publishes exactly one prepared face and retention charge;
- repeated get and lookup return the identical shared handle;
- clear releases cache retention without invalidating caller handles;
- post-clear get prepares a distinct new native face;
- sixteen simultaneous misses publish exactly one face;
- two-entry LRU retains the touched entry and evicts the true victim;
- invalid bindings clear previous output;
- oversized source retention is rejected before preparation;
- one-byte metadata budget rejects in-flight admission atomically;
- strict test repetition, Linux ASan/UBSan, and all prior shaping gates pass.

## Explicit boundary

This slice does not automatically prepare during shaping, maintain a global
singleton, estimate native HarfBuzz heap bytes, persist cache state, share
entries across processes, cache `hb_font_t`/`hb_buffer_t`, cache shape plans, or
perform asynchronous preparation. Automatic cache-backed catalog shaping is a
separate integration slice.
