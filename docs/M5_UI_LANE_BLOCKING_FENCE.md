# M5 — UI-Lane Blocking Layout Fence

## Problem

The current `ZenithHotScrollSession::layout()` implementation can still perform synchronous filesystem work when a persistent checkpoint or source window is absent from its hot caches. `ZenithTabRuntime::layout()` previously measured that call as visible UI work and then submitted a scheduler charge marked `lane=Ui` and `may_block=false`. That accounting was not an honest execution contract: a source-window miss can call `StoreReader::read_record_slice()` and a checkpoint miss can perform filesystem existence/open work.

## Fence

`ZenithTabRuntime` now exposes `layout_on_lane()` with an explicit `FrameExecutionLane`.

For a visible tab:

- `FrameExecutionLane::Ui` is rejected before ready-prefetch draining, checkpoint lookup, source-window lookup, or `ZenithHotScrollSession::layout()`;
- the rejection is passed through `FrameBudgetScheduler` as a visible request with `may_block=true`, so scheduler authority must return `BlockingOnUi`;
- `ui_blocking_layout_rejections` records the fail-closed event;
- the returned layout result is empty and no checkpoint path is reported.

The existing `layout()` API remains available as the synchronous compatibility entry point, but it now delegates explicitly to `FrameExecutionLane::Worker`. Its scheduler accounting is correspondingly marked `lane=Worker` and `may_block=true`.

## Validation contract

The focused tab-runtime test records hot-scroll counters before a visible UI-lane request and requires all of the following:

- the request fails closed with the UI-lane reason;
- `ui_blocking_layout_rejections` increments exactly once;
- scheduler rejected-request accounting increments;
- hot-scroll `layout_calls` does not change;
- checkpoint-cache miss accounting does not change;
- source-window-cache miss accounting does not change;
- the same runtime can still execute the synchronous worker-lane compatibility layout and produce accelerated fragments.

## Admission boundary

This slice removes the false claim that the existing synchronous hot-scroll layout is UI-safe. It is **not** full M5 non-blocking-layout credit.

The next production slice must provide a bounded asynchronous foreground-layout handoff or a cache-only layout path that can return usable UI work without synchronous checkpoint/source I/O. Until that exists, visible UI callers must not invoke the blocking compatibility path.
