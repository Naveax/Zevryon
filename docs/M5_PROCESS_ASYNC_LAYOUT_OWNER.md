# M5 — Process Async Foreground Layout Owner

## Purpose

`ZenithProcessRuntimeServices` owns the foreground-layout worker pool alongside the existing process-shared source-prefetch pool and record-length authority.

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

## Critical pressure and runtime retirement

A hidden runtime receives the critical activity transition first, which invalidates foreground authority and requests a hot-scroll critical trim without waiting on the active layout mutex.

The runtime is then retired from public/materialized tab ownership rather than destroyed synchronously. Runtime generations use internal pool identities that are distinct from public tab identities, so a rematerialized or reopened public tab can receive a fresh runtime generation while an older callback still owns the retired generation it started with.

Retired generations are destroyed only after process foreground callbacks are no longer running. This keeps critical-pressure and explicit-close public lifecycle paths from waiting on foreground layout execution.

## Dormant registry invariant

Hidden slots that have never been materialized still consume only lightweight registry metadata. They create neither source-prefetch sessions nor foreground-layout sessions. Failed materialization must leave both shared pools unchanged.

## Lifecycle accounting

`retired_runtime_generations` reports old runtime generations retained solely for callback lifetime safety. They are not materialized tabs and have already received hidden/critical runtime authority before retirement.

The detailed generation contract is recorded in `M5_RUNTIME_GENERATION_RETIREMENT.md`.
