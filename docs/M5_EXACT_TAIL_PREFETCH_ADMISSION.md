# M5 — Exact Tail Prefetch Admission

## Purpose

Velocity-aware lookahead can legitimately request a full source window whose starting offset is still inside a record but whose remaining bytes before EOF are smaller than the requested maximum. `StoreReader::read_record_slice()` correctly returns that shorter successful payload.

Before this slice, `ZenithTabRuntime` forwarded the original larger `max_bytes` to `ZenithHotScrollSession::admit_prefetched_source_window()`. Exact admission then rejected the short payload because the returned byte count did not match the requested cache key. The I/O had completed successfully but its bytes could not enter the authoritative hot-scroll cache.

## Canonicalization contract

`canonicalize_prefetch_tail_for_exact_admission()` narrows only successful, non-empty short results:

- full result: request key is unchanged;
- successful short result: `request.max_bytes` becomes the exact returned byte count;
- failed, empty, oversized, zero-request, or null result: fail closed.

The byte offset and physical record identity never change.

After canonicalization, the existing hot-cache admission contract remains authoritative. No partial payload is ever inserted under a larger key.

## Resource behavior

This adds no worker, queue, ready slot, tab limit, or cache. It merely allows an already-returned EOF tail to transfer from the bounded shared ready-result slot into the existing bounded hot-scroll source LRU.

The shared pool still relinquishes the ready-result memory before the payload is moved into the hot cache, so there is no duplicate resident copy introduced by this handoff.

## Boundary

This slice makes short EOF reads useful; it does not yet prevent the speculative read from being issued at a size larger than the remaining record tail. Preventing that requires record-length authority to reach prediction without creating one metadata arena or descriptor cache per tab. That authority should be process-shared or carried from the existing hot-scroll materialization path, not reconstructed independently per tab.

## Focused validation

The helper test covers:

- unchanged complete windows;
- successful short-result canonicalization to an exact tail key;
- failed results;
- empty EOF results;
- oversized payload corruption;
- null input.
