# M5 — Process Async Foreground Layout Owner

## Purpose

`ZenithProcessRuntimeServices` now owns the foreground-layout worker pool alongside the existing process-shared source-prefetch pool and record-length authority.

Materialized tabs receive one shared `SharedForegroundLayoutWorkerPool`; dormant hidden tab slots receive no foreground worker session and therefore do not create worker threads or retain ready viewport payloads.

## Process bounds

The process owner configures three independent foreground bounds:

- `foreground_layout_worker_count` — fixed process worker count, hard-capped at 64;
- `foreground_layout_ready_bytes` — process-wide retained ready-result budget;
- `foreground_layout_max_fragments` — per-request fragment-count ceiling.

The worker pool remains lazy: no foreground threads start until the first runtime is materialized.

## UI boundary

The process owner exposes `request_tab_layout_async()` and `try_take_tab_layout_async()` so an embedder can submit and consume visible viewport work without calling synchronous hot-scroll layout on the UI lane.

Dormant or dematerialized tabs reject foreground requests as inactive. Unknown session identities fail closed.

## Critical pressure and runtime destruction

A hidden runtime receives the critical activity transition first, which invalidates foreground authority and requests a hot-scroll critical trim without waiting on the active layout mutex.

Destroying that runtime closes its foreground worker session. To prevent the process event-loop pressure path from waiting on an already-running foreground callback, hidden critical runtime destruction is deferred while any process foreground callback is running. A later process tick or activity transition drains the pending destruction once the foreground worker set has no running session.

This is deliberately conservative across tabs: unrelated running foreground work may delay hidden-runtime destruction briefly, but the event-loop path remains non-blocking with respect to foreground layout execution.

## Dormant registry invariant

Hidden slots that have never been materialized still consume only lightweight registry metadata. They create neither source-prefetch sessions nor foreground-layout sessions. Failed materialization must leave both shared pools unchanged.

## Remaining boundary

Explicit user-driven `close_tab()` still synchronizes runtime destruction with an already-running callback through the worker-pool close contract. Making explicit close fully asynchronous is a separate lifecycle slice and is not claimed here.
