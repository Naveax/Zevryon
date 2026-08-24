# M5 — Unbounded Tab Registry Policy

## Goal

Zevryon must not impose a finite product-level tab-count ceiling. The browser may keep as many tabs registered as process address space and host memory can naturally sustain. Performance authority is therefore expressed as bounded active resources, not as a fixed maximum number of tabs.

The historical shared-prefetch session ceiling is now removed from the configuration contract entirely. There is no `max_sessions`, no replacement tab-count constant, and no hidden finite registry admission knob.

## Resource contract

Tab registry cardinality and expensive resources are deliberately decoupled:

- registering an idle tab starts zero native prefetch worker threads;
- shared prefetch worker count remains bounded by `worker_count` and does not scale with tab count;
- speculative ready-result payload retention is bounded globally by `max_ready_bytes`;
- each session owns at most one pending speculative request and one ready result;
- hidden/inactive sessions cannot schedule speculative work;
- shared-pool telemetry is O(1) with respect to registered session count;
- default built-in `StoreReader` lifetime is bounded to one worker execution, so concurrently live readers are bounded by worker count rather than by the number of tabs that have ever prefetched;
- process memory pressure, visibility, and activity determine reclamation behavior. Tab count itself is not a pressure signal.

Registry insertion can still fail naturally if the process cannot allocate the small bookkeeping object or if the supplied session identity already exists. Those are physical/correctness failures, not a product policy ceiling.

## Regression proof

`unbounded-tab-registry-tests` opens 4096 inactive sessions. The number 4096 is only a CI regression sample chosen to be far beyond the historical finite session policy; it is not a new limit.

The test requires:

- all 4096 unique registrations succeed;
- a duplicate session identity is rejected;
- accounting reports all registrations;
- active session count remains zero;
- shared worker thread count remains zero;
- speculative ready bytes/results remain zero;
- all sessions close cleanly without starting workers.

## Meaning of "unbounded"

"Unbounded" means Zevryon contains no finite browser-policy constant such as 100, 256, 4096, 10000, or any configurable session maximum that decides whether another tab may register. Host memory, address space, allocator failure, and operating-system limits remain physical constraints on any finite computer.

Expensive resources remain explicitly bounded independently of registry cardinality. Removing the artificial tab wall therefore does not mean removing RAM, thread, I/O, cache, or pressure-control discipline.
