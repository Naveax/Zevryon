# M5 — Velocity-Aware Runtime Prefetch

## Purpose

This slice wires the bounded velocity planner into `ZenithTabRuntime` without changing shared worker count, per-session queue cardinality, or global ready-result memory limits.

`ZenithTabRuntime::choose_prefetch_request()` now combines the scheduler's authoritative direction ticket with the current signed scroll velocity. The selected source edge is still the last visible fragment for forward scrolling and the first visible fragment for reverse scrolling.

## Lead policy

With the default 64 KiB source window:

- slow motion preserves the previous request offset exactly;
- medium motion targets one additional 64 KiB window ahead;
- fast motion targets three additional 64 KiB windows ahead.

Reverse scrolling applies the same bounded distance in the opposite source direction.

This changes prediction distance only. It does not create multiple requests, multiple ready results, additional worker threads, or per-tab worker pools.

## Authority and cancellation

The existing `PrefetchTicket` remains authoritative. Direction changes and stops already advance/invalidate the scheduler epoch and the shared pool rejects stale requests/results.

The new planner adds another fail-closed check: the ticket direction and signed runtime velocity must agree. A stale or contradictory direction/velocity pair therefore cannot issue speculative I/O even before pool authority validation.

## Overflow and source-bound behavior

Forward lead arithmetic rejects requests whose predicted source offset would overflow `uint64_t`. Reverse lead arithmetic saturates its distance and clamps at source offset zero. Reverse prefetch is suppressed when the visible source start is already zero.

A future slice may add record-length-aware forward tail clamping so a fast prediction near end-of-record can avoid a short speculative read. Exact hot-cache admission remains fail-closed for short results in the meantime.

## Resource invariants

Unchanged invariants:

- process-level tab registry has no finite policy cap;
- shared worker count remains fixed;
- one pending and one ready slot per session remain bounded;
- global ready bytes remain bounded;
- hidden tabs issue no speculative work;
- critical pressure suppresses speculative scheduling;
- exact source-window cache admission retains its correctness contract.

## Focused validation

The velocity planner test now covers directional source offsets in addition to tier selection:

- slow forward preserves legacy edge behavior;
- fast forward applies the expected bounded lead;
- medium reverse applies symmetric lead;
- direction/velocity sign mismatch fails closed;
- reverse prediction cannot cross source start;
- extreme signed velocity and lead arithmetic remain overflow-safe.
