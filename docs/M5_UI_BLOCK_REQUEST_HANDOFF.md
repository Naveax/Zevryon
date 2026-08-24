# M5 — Actionable UI Would-Block Handoff

## Authority

This design follows the fail-closed hot-scroll contract introduced at:

`b13953d32961835debde71396cc4fedcf57b0808`

and the deterministic height-persistence regression child at:

`703bd1b66aac26cac362885a5b97fbea8b367723`

The parent M5 integration remains draft. Hosted CI success does not itself satisfy the physical frame/thermal admission boundary.

## Why another slice is required

The first cache-only API correctly prevents synchronous checkpoint discovery/open, source reads, and arena-height persistence from occurring on a UI-accounted layout call. That is necessary, but not sufficient for production integration.

A `WouldBlock*` status without the exact work identity is not actionable. If `ZenithTabRuntime::layout()` switched to the cache-only call today, a miss could be detected but the runtime would not have enough structured information to ask a bounded worker to satisfy every class of miss. Repeatedly retrying the same cache-only request would simply starve the frame.

Therefore production UI integration must wait until each fail-closed outcome carries a deterministic worker request and a stale-result authority rule.

## Required request identity

Introduce an explicit hot-scroll block request whose kind matches the status and whose payload is fully sufficient for worker execution.

### Checkpoint request

Must carry at least:

- source record index;
- logical record id;
- source byte length;
- exact `LayoutCheckpointConfig` used by the request;
- a tab/session authority epoch.

The worker may perform checkpoint path discovery/open. Publication back to the tab must validate the complete source identity and layout configuration before admitting the parsed `LayoutCheckpointIndex` into the existing bounded checkpoint LRU.

A width-only cache key is not enough for externally admitted checkpoints. Average advance, line height, padding and checkpoint stride must also match the active layout configuration or admission must fail closed.

### Source-window request

Must carry at least:

- source record index;
- exact byte offset;
- exact requested byte count;
- the current prefetch/scroll authority ticket or a dedicated layout-miss epoch.

The existing `SharedSourcePrefetchPool` plus `admit_prefetched_source_window()` already provides most of this machinery. A mandatory visible-layout miss must, however, be distinguishable from speculative velocity prefetch so speculative replacement/coalescing policy cannot indefinitely displace correctness-critical visible work.

### Height-persistence request

Must carry at least:

- logical record index;
- source record index;
- expected old height;
- requested measured height;
- expected logical-order generation;
- enough block/arena authority to reject a stale write.

This is the hardest class because `CompactArenaReader::update_height()` currently couples:

1. persisted source-height read/validation;
2. source height write and flush;
3. height-block write and flush;
4. arena header write and flush;
5. in-memory order-statistics sequence update;
6. in-memory Fenwick/block/total-height publication.

Moving only the filesystem calls to another thread is not sufficient if the UI-owned reader then races or diverges from the persisted result.

## Height update split required before runtime wiring

Refactor height correction into an authority-preserving two-phase operation.

### Phase A — prepare on the owning runtime

A non-blocking preparation step derives an immutable request from current resident state and captures the expected generation/old height. It performs no filesystem I/O.

### Phase B — persist on a bounded worker

The worker validates the persisted old height/generation and writes the source height, block height and arena header transactionally with the existing rollback behavior. It returns a completion object containing the exact expected-old/new values and authority generation.

### Phase C — publish resident state on the owning runtime

The UI/runtime consumes the completion only if its logical-order generation and current resident old height still match the request. It then updates the order-statistics sequence, Fenwick block and total height **without filesystem access**. Stale completions are discarded and re-planned.

This preserves one owner for resident layout state while keeping physical disk work off the UI lane.

## Worker ownership

Do not create a worker thread per tab.

Checkpoint-open and height-persistence work should use a process-shared bounded worker service, parallel to the existing shared source-prefetch service. At minimum it needs:

- bounded worker count;
- bounded ready-result memory;
- per-session latest authority epoch;
- one pending correctness-critical request per work kind/session or another explicitly bounded queue;
- stale/inactive/closed-session result rejection;
- hidden/critical-pressure cancellation where correctness permits;
- visible correctness work priority above speculative prefetch.

## Production `ZenithTabRuntime::layout()` sequence

Once actionable requests and worker completion admission exist:

1. drain ready mandatory-layout completions;
2. drain/admit ready speculative source prefetch;
3. run `layout_nonblocking()` only;
4. on `Ready`, publish visible layout and then schedule speculative prefetch within remaining budget;
5. on `WouldBlockCheckpoint`, enqueue/coalesce exact checkpoint work and return a non-corrupt pending-layout state;
6. on `WouldBlockSource`, enqueue correctness-critical exact source work using bounded shared workers;
7. on `WouldBlockHeightPersistence`, enqueue the prepared height transaction;
8. never call the synchronous hot-scroll fallback from the UI lane;
9. keep the synchronous layout API available for worker/offline correctness contexts and tests.

## Admission gates

Production UI wiring is admitted only when focused tests prove all of the following:

- every `WouldBlock*` outcome contains exact actionable identity;
- stale checkpoint/source/height completions are rejected;
- hidden and closed tabs cannot publish stale ready work;
- source correctness work cannot be starved by speculative velocity prefetch;
- height persistence cannot race logical reorder or a newer height correction;
- cache-only UI layout performs zero filesystem operations on every miss class;
- existing synchronous correctness behavior remains intact;
- process worker count and ready memory remain bounded independently of tab count.

Only after these are structural properties should the UI lane receive the literal M5 non-blocking credit.
