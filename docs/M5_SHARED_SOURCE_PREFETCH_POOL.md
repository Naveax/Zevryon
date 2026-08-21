# M5 — Shared Cross-Tab Source Prefetch Pool

## Purpose

This slice replaces the assumption that every page/session needs its own speculative source-prefetch execution thread with a bounded, explicitly shared execution primitive.

The target is many-tab behavior: opening or visiting more pages may increase session metadata, but it must not linearly increase native prefetch threads or allow one noisy tab to monopolize speculative I/O.

## Pool contract

`SharedSourcePrefetchPool` owns a fixed worker bound, a bounded session registry, one latest pending request per session, and one ready result per session.

The default policy is:

- 2 shared worker threads;
- at most 256 registered sessions;
- at most 2 MiB of retained ready-result payload across the whole pool;
- no worker threads before the first valid accepted prefetch request.

The configured worker count is capped at 64 and is independent of the number of registered sessions.

## Cross-tab fairness

Each session can have at most one pending speculative request. A newer request for the same session replaces the older pending request instead of growing a FIFO.

After a session finishes one request, any new pending work for that same session is requeued at the back of the shared queue. With a single worker this creates deterministic round-robin behavior across ready sessions; with multiple workers it still prevents one session from filling the queue with an unbounded burst.

This is deliberately a fairness rule for speculative source work, not a general browser task scheduler.

## Hidden and inactive sessions

A session has an explicit authority ticket and active flag. Moving a tab to an inactive/hidden state can therefore:

- reject new speculative requests immediately;
- cancel stale pending work;
- invalidate stale ready results;
- allow already-running bounded I/O to finish safely while dropping its result instead of publishing it into a hidden page;
- resume later only under a fresh authority ticket.

This stacks with the frame-budget hidden-tab suppression contract. The frame scheduler decides that hidden tabs receive no frame work; the shared prefetch pool independently guarantees that speculative source results cannot leak across a visibility/authority transition.

## Memory bound

Ready result payload is charged against one global pool budget using retained vector capacity. A result that would exceed the global budget is simply dropped because prefetch is optional. Correct foreground reads remain authoritative and are not blocked by speculative-cache pressure.

Each session retains at most one ready result. Replacing a same-session ready result first removes the old charge and then publishes the new result only if the global budget still admits it.

## Store behavior

The default executor keeps one lazily opened `StoreReader` per registered session. Only one pool task for a given session can run at a time, so that reader is not concurrently accessed by multiple pool workers.

A test executor hook remains available for deterministic scheduling, cancellation, and fairness tests without weakening the production default path.

## Tests

The new shared-pool test target proves:

- invalid/stale traffic starts zero worker threads;
- first accepted work starts only the configured fixed worker count;
- thread count does not scale with session count;
- one noisy session cannot run its requeued work before another already-ready session;
- same-session pending requests coalesce/replace instead of growing the queue;
- hidden/inactive transitions drop running stale results and reject new work;
- a resumed session can publish under a fresh authority ticket;
- ready-result memory never exceeds the global hard budget;
- session-count limits are enforced and closed slots are reusable;
- the real `StoreReader` path returns exact bounded bytes.

## Boundary

This slice introduces the shared execution primitive but does not yet replace every existing `SourceWindowPrefetchWorker` construction site. The next integration step is to make browser/tab ownership use one process-level `SharedSourcePrefetchPool`, then retire the dedicated-worker compatibility path after equivalence and performance evidence are green.

It also does not claim complete Live100 scheduling. JavaScript timers, network fetches, media, compositor surfaces, process priority, service workers, and OS-level suspension remain separate contracts.
