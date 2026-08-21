# M5 — Unbounded Tab Registry Policy

## Goal

Zevryon must not impose a finite product-level tab-count ceiling. The browser may keep as many tabs registered as process address space and host memory can naturally sustain. Performance authority is therefore expressed as bounded *active resources*, not as a fixed maximum number of tabs.

This replaces the historical shared-prefetch default of 256 sessions. The default registry policy now uses `SIZE_MAX`, which removes the artificial browser-level admission wall without pretending a finite machine can literally allocate infinite objects.

## Resource contract

Tab count and heavy resource count are deliberately decoupled:

- registered/idle tab metadata may grow with the number of tabs;
- merely registering a tab starts **zero** native prefetch worker threads;
- shared prefetch worker count stays fixed by `worker_count` and does not scale with tab count;
- speculative ready-result payload retention stays bounded globally by `max_ready_bytes`;
- each session still owns at most one pending request and one ready result;
- hidden/inactive sessions cannot schedule speculative work;
- background/critical memory-pressure trimming remains responsible for releasing tab-local hot-scroll working sets.

A low-level `max_sessions` field is retained temporarily for embedding and focused test harnesses, but its default is `std::numeric_limits<std::size_t>::max()` and it is **not** the Zevryon browser tab policy. The next process-level pressure-controller slice should remove any dependence on finite tab counts and operate entirely on activity, recency, visibility, and memory pressure.

## Regression proof

`unbounded-tab-registry-tests` opens 4096 inactive sessions using the default policy. The number 4096 is only a CI regression sample chosen to be far beyond the historical 256-session default; it is not a new limit.

The test requires:

- all 4096 registrations succeed;
- session accounting reports all registrations;
- active session count remains zero;
- native shared-prefetch worker thread count remains zero;
- speculative ready bytes/results remain zero;
- all sessions close cleanly without starting workers.

## Meaning of "unbounded"

"Unbounded" means Zevryon contains no finite browser-policy constant such as 100, 256, 1000, or 10000 tabs. Host memory, address space, operating-system object limits, and allocation failure are physical limits and must be handled gracefully, but they are not product tab-count limits.

The architecture must therefore keep expensive resources bounded independently from registry cardinality. This is the basis for the upcoming process-level pressure controller.
