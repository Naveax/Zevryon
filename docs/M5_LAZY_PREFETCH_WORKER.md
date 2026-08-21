# M5 — Lazy Source-Prefetch Worker Startup

## Problem

The bounded source-window prefetch worker previously created its dedicated `std::thread` in the constructor. That made construction itself carry a native-thread cost even when a page never issued one valid speculative source request.

In a future many-tab browser this is an undesirable default: an inactive or never-scrolled tab should not acquire an idle prefetch thread merely because its session object exists.

## Change

`SourceWindowPrefetchWorker` now creates no thread in either constructor. The worker thread is started lazily only when the first request has passed all admission checks and is about to become pending work.

These operations remain thread-free:

- construction;
- authority-ticket updates;
- status queries;
- invalid zero-byte requests;
- requests larger than the bounded I/O window;
- stale or stationary prefetch requests;
- stopping/destroying a worker that never accepted work.

The first accepted request starts exactly one worker thread. Later accepted, coalesced, replaced, or authority-changing requests reuse that thread. No per-request thread creation is introduced.

## Telemetry

`SourceWindowPrefetchStatus` adds:

- `thread_started`: whether this worker has ever needed its execution thread;
- `thread_starts`: number of thread starts for the worker lifetime.

The current contract requires `thread_starts <= 1` for a worker that has accepted work and `0` for a worker that never accepted work.

## Multi-tab effect

This removes eager native-thread acquisition from cold/inactive prefetch sessions. A browser may therefore create page/session state without automatically scaling idle prefetch threads with the number of open tabs.

This stacks with two separate controls:

1. hidden-tab frame scheduling suppresses new prefetch admission at the scheduling layer;
2. hot-scroll pressure trimming releases source-window and transient memory for background tabs.

Together these reduce CPU scheduling and memory retention before the later cross-tab worker-pool/process scheduler exists.

## Tests

The existing source-prefetch test target now proves:

- construction starts zero threads;
- authority changes start zero threads;
- invalid and stale-only traffic starts zero threads;
- the first accepted request starts one thread;
- a second accepted request reuses the same thread;
- real StoreReader-backed prefetch remains correct;
- pending replacement, epoch reversal, stale-result dropping, and ready invalidation continue to use one worker thread.

## Boundary

After a worker has accepted real work, this slice keeps its thread alive until normal `stop()`/destruction. Automatic idle retirement and a shared cross-tab executor pool are intentionally not claimed here. Those require a separate lifecycle/fairness contract so thread retirement cannot race request publication or weaken the existing stale-result guarantees.
