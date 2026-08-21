# M5 — Shared Cross-Tab Source Prefetch Pool

## Purpose

`SharedSourcePrefetchPool` provides one process-level speculative source-I/O scheduler for many tabs. Opening more tabs may add lightweight registry metadata, but it must not linearly add native worker threads, retained speculative payload, or per-tab native readers.

## Pool contract

The registry has no finite session-count policy cap. Session identity must be unique while registered, but there is no `max_sessions` field or equivalent admission constant.

Expensive resources remain bounded independently:

- default shared worker count: 2;
- worker count hard maximum: 64;
- default global retained ready-result budget: 2 MiB;
- at most one latest pending request per session;
- at most one ready result per session;
- zero worker threads before the first accepted valid speculative request.

## Cross-tab fairness

A newer pending request for one session replaces that session's older pending request rather than growing an unbounded FIFO. After one request finishes, any new pending work for the same session is requeued at the back of the shared queue. This prevents one noisy tab from monopolizing speculative I/O while preserving a globally bounded worker population.

## Hidden and inactive sessions

Each session has an authority ticket and active flag. Hidden/inactive transitions reject new speculative work, cancel stale pending authority, invalidate stale ready results, and drop already-running results rather than publishing them into a hidden page. A resumed tab must use fresh authority.

This composes with the frame scheduler: hidden tabs receive no frame work, while the pool independently prevents speculative source results from crossing a visibility/authority transition.

## Memory and reader bounds

Ready payload is charged against one global `max_ready_bytes` budget. A speculative result that would exceed that budget is dropped; foreground reads remain authoritative.

The built-in executor no longer retains one lazy `StoreReader` per session. A reader exists only for one worker execution. Consequently concurrently live built-in readers are bounded by `worker_count`, not by registered tab count or the number of tabs that previously prefetched.

The V2 executor contract can canonicalize speculative offset/length after worker-side record-length resolution while preserving immutable record identity and authority ticket. EOF work can therefore be shortened or suppressed before payload I/O without publishing bytes under a false cache key.

## Telemetry scaling

Active, queued, running, and ready counts are maintained incrementally. `status()` is O(1) with respect to registry cardinality. `wait_idle_for()` intentionally retains a full-state coordination check because it is not the telemetry hot path.

## Tests

Focused tests cover:

- invalid/stale traffic starts zero workers;
- first accepted work starts only the configured fixed worker count;
- thread count does not scale with registered sessions;
- round-robin latest-pending fairness;
- hidden/inactive cancellation and fresh-authority resume;
- global ready-result memory budget;
- policy-unbounded session registration with duplicate-identity rejection and close/reopen reuse;
- real `StoreReader` bounded reads;
- V2 canonical request identity and fail-closed identity-rewrite rejection;
- worker-side EOF canonicalization/suppression with process-shared record metadata;
- O(1) status counters across a 4096-session regression sample.

The 4096-session samples are test sizes, not product limits.

## Integration boundary

`ZenithTabRuntime` now uses the process-level shared pool, including visibility authority, velocity-aware prefetch, exact cache admission, process-shared record bounds, worker-side EOF canonicalization, and device-specific frame scheduling. Broader browser subsystems such as JavaScript timers, network fetch scheduling, media surfaces, service workers, compositor resources, and OS suspension remain separate contracts rather than being falsely claimed by this pool.
