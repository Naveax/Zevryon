# M5 — Zenith Tab Runtime Integration

## Purpose

This slice connects the current hot-scroll engine, frame-budget authority, memory-pressure trimming, and the process-level shared source-prefetch pool behind one tab/session ownership object: `ZenithTabRuntime`.

The goal is to make the multi-tab policies executable rather than leaving them as independent primitives.

## Ownership

A runtime owns one `ZenithHotScrollSession` and one `FrameBudgetScheduler`. It registers one session identity with a caller-owned `SharedSourcePrefetchPool`; the pool is expected to outlive the runtime.

The runtime remains correct when no shared pool is supplied. Speculative prefetch is optional and can never become a dependency of layout correctness.

## Visible frame path

For a visible tab, `layout()`:

1. drains the previous bounded shared-prefetch result, releasing its global ready-memory charge after the speculative read has already warmed the operating-system file cache;
2. executes the authoritative hot-scroll layout synchronously;
3. measures real elapsed wall time with `steady_clock`;
4. charges the visible frame class by the measured duration, capped at the configured frame budget;
5. closes the visible phase;
6. only if frame budget remains, pressure is not critical, scrolling has a current non-zero authority ticket, and a fragment exists, admits one bounded worker-lane prefetch request.

If visible layout consumes or overruns the frame budget, speculative source work is not admitted for that frame.

This slice intentionally uses prefetch as page-cache warming. It does not yet inject prefetched payload bytes into the private hot-scroll source-window LRU. That later handoff can reduce counted synchronous source reads, but it requires a separate exact-key cache-admission contract and must not be conflated with this ownership/scheduling slice.

## Physical source identity

`LayoutFragment` now carries `source_record_index` on the hot-scroll path in addition to the mutable logical `record_index`.

That distinction is mandatory after logical reorder: the visible fragment may have logical ordinal zero while its immutable payload is physical source record one. Prefetch requests always use `source_record_index`; they never infer StoreReader identity from logical order.

Existing generic `LayoutWindowEngine` callers do not rely on this new field. The field is introduced for hot-scroll/tab-runtime source authority and is populated where the physical `MaterializedRecord` is available.

## Hidden tab path

When activity becomes hidden:

- scroll motion is neutralized, advancing the prefetch epoch when needed;
- the shared-pool session becomes inactive, cancelling stale pending/ready work and causing already-running speculative results to be dropped on completion;
- normal/elevated hidden pressure invokes background hot-scroll trim, releasing source windows and transient source/fragment scratch while preserving parsed checkpoint metadata;
- critical hidden pressure additionally releases parsed checkpoint cache;
- `ZenithTabRuntime::layout()` returns an empty suppressed result without entering `ZenithHotScrollSession::layout()` at all.

Returning visible issues fresh scroll-prefetch authority before new speculative work can be admitted.

## Test contract

The integration test builds a real segmented store, compact arena, and persistent layout checkpoints, then proves:

- hot-scroll fragments preserve immutable physical source identity across logical record reorder;
- registering a tab starts zero shared worker threads;
- the first visible layout can schedule one bounded shared prefetch under spare frame budget;
- a running prefetch is not published after the tab becomes hidden;
- background hiding trims hot-scroll source/scratch working set;
- hidden layout does not increment the underlying hot-scroll layout-call counter;
- visible resume receives a fresh prefetch epoch and can schedule again;
- ready speculative results are drained under bounded accounting;
- critical hiding releases both source-window and checkpoint cache state;
- the executor sees physical source record identity, not the mutable logical ordinal.

## Boundaries

This is not M5 or Live100 completion. Remaining work includes exact prefetched-byte admission into the hot-scroll source LRU, browser-shell ownership of one process-level pool, automatic pressure inputs, timer/network/media policy, compositor scheduling, renderer process priority, service workers, and OS suspension.
