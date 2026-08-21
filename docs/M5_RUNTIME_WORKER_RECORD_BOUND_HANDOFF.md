# M5 — Runtime Worker Record-Bound Handoff

`ZenithTabRuntime` now includes the real visible source edge in each speculative request after applying any cache-only UI-side record bound.

This closes the handoff between velocity prediction and the shared prefetch executor V2:

- UI thread uses only already-cached record metadata and never blocks on descriptor I/O;
- the request carries `visible_edge_offset` plus an explicit validity bit;
- on a record-length cache miss, the shared worker may resolve the length and safely re-clamp the prediction;
- the worker publishes its canonical offset/size with the returned bytes;
- EOF suppression can happen before payload I/O;
- physical record identity and `PrefetchTicket` cannot be rewritten.

The visible edge is a scheduling hint, not a cache identity component. Exact hot-cache identity remains `(physical record, byte offset, exact byte count)` after canonicalization.
