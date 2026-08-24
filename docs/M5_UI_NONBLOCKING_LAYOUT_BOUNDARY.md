# M5 — UI Non-Blocking Layout Boundary

## Authority

This execution slice starts from exact M5 candidate head:

`90868d0a61ad346dcf6b8e286325df1b73ed95c1`

The parent exact-head Windows/Linux CI remains independent. This child branch deliberately uses the `candidate/**` namespace so writing the execution spec does not create a second Actions run while the parent run is still active.

## Blocker found by fresh source audit

`ZenithTabRuntime::layout()` accounts visible work as `FrameExecutionLane::Ui` and sets `may_block = false`, but that label is not yet enforced by the actual hot-scroll call graph.

The visible call currently enters `ZenithHotScrollSession::layout()`, which can perform all of the following synchronously on a cache miss or geometry correction:

1. `std::filesystem::exists()` for persistent checkpoint discovery;
2. `LayoutCheckpointIndex::open()` for checkpoint file loading;
3. `StoreReader::read_record_slice()` for source-window reads;
4. `CompactArenaReader::update_height()` for persisted height/index/header writes and flushes.

Therefore exact head `90868d0a61ad346dcf6b8e286325df1b73ed95c1` has strong scheduling, prefetch, cache admission, pressure, many-tab and evidence infrastructure, but it does **not** yet satisfy the literal M5 invariant "no blocking disk on the UI thread" for every visible layout call.

A scheduler request marked `may_block = false` is accounting metadata; it cannot make a synchronous filesystem operation non-blocking.

## Required fail-closed contract

The next implementation child must introduce an explicit cache-only hot-scroll layout mode before production `ZenithTabRuntime` is allowed to claim non-blocking UI credit.

The cache-only path must distinguish at least these outcomes:

- `Ready` — all data needed for the requested layout is already resident and no persistence is required;
- `WouldBlockCheckpoint` — a required parsed checkpoint is not resident;
- `WouldBlockSource` — a required exact source window is not resident;
- `WouldBlockHeightPersistence` — authoritative geometry would require a persisted arena-height update;
- ordinary hard failure — invalid/corrupt state unrelated to scheduling readiness.

`WouldBlock*` is not a correctness failure and must not silently fall through to synchronous I/O on the UI lane.

## Compatibility boundary

The existing synchronous `ZenithHotScrollSession::layout()` remains the correctness fallback for explicitly blocking/worker contexts. The new cache-only mode is additive and must not weaken source-byte, logical-order, checkpoint, search, export, or M3/M4 authority.

No existing blocking fallback may be relabeled as non-blocking merely because speculative prefetch often warms the operating-system cache.

## First implementation acceptance

A focused regression must prove:

1. a fresh session with an on-disk checkpoint but no parsed checkpoint returns `WouldBlockCheckpoint` without opening/checking the checkpoint path from the cache-only call;
2. after an explicitly blocking warm-up, the same layout returns `Ready` from cache with zero physical source bytes read;
3. after clearing only the source-window cache, the cache-only call returns `WouldBlockSource` and performs zero `StoreReader` source I/O;
4. a geometry mismatch that would call `CompactArenaReader::update_height()` returns `WouldBlockHeightPersistence` before any persisted write;
5. the legacy synchronous layout behavior and existing hot-scroll tests remain unchanged.

## Production integration after the contract exists

Only after the cache-only boundary is deterministic should `ZenithTabRuntime::layout()` switch its UI path to it. `WouldBlockCheckpoint`, `WouldBlockSource`, and `WouldBlockHeightPersistence` then need worker-side fulfillment while preserving:

- visible-first frame-budget authority;
- one process-shared bounded worker system rather than per-tab workers;
- stale ticket cancellation;
- exact source-window cache identity;
- process-shared record-length authority;
- hidden-tab suppression/dematerialization;
- pressure-driven cache shrinking;
- synchronous correctness fallback only outside the UI lane.

## Admission rule

Hosted Windows/Linux CI can certify builds, tests, Unicode authority and exact-head identity. It cannot by itself award the missing UI non-blocking credit until the call graph is structurally prevented from doing blocking filesystem work and the cache-only/worker handoff is covered by focused tests.
