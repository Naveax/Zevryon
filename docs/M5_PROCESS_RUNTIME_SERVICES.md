# M5 — Process Runtime Services Owner

## Purpose

The many-tab runtime now has one explicit process owner for resources that must not be duplicated per tab.

`ZenithProcessRuntimeServices` owns:

- one process-shared record-length authority;
- one bounded shared source-prefetch pool;
- one tab pressure controller;
- one stateful memory-pressure policy;
- one adaptive process memory sampler;
- the registered `ZenithTabRuntime` objects.

The owner does not expose a finite tab-count policy. Registered runtime metadata may grow until the process naturally cannot allocate more state.

## Resource lifetime

The process owner constructs the shared prefetch pool with an internal pointer to its own record-length authority. Embedders do not supply that raw pointer, which prevents an accidental dangling authority lifetime.

Heavy resources remain bounded independently of tab count:

- `prefetch_worker_count` is capped at 64;
- global retained ready payload uses `prefetch_ready_bytes`;
- record-length metadata uses its bounded process-wide LRU;
- memory sampling is process-scoped and threadless;
- hidden tabs are immediately registered with inactive speculative authority.

## Tab lifecycle

`open_tab()` creates a profile-specific `ZenithTabRuntime`, registers its shared-pool session, stores the runtime in the process registry, and binds it to the process controller.

`close_tab()` unregisters controller authority before destroying the runtime so the shared-pool session closes with the runtime lifetime.

Duplicate live session identities are rejected. A closed identity may be reused. This is an identity rule, not a cardinality limit.

## Event-loop bridge

`on_event_loop_tick(monotonic_ms)` forwards process cadence to the adaptive memory sampler. Early ticks return `Throttled` without operating-system memory calls. Pressure transitions are applied through the shared controller, so hidden tabs are reclaimed before visible tabs while visible foreground layout remains authoritative.

## Validation

The focused integration test builds a real MassiveDoc store, compact arena, and layout checkpoint, then opens one hidden and one visible tab through the process owner. It verifies:

- two tabs share one pool and open with zero worker threads;
- only the visible tab has active speculative authority;
- duplicate identity is rejected;
- a critical process-memory sample trims the hidden runtime;
- the visible runtime still performs foreground layout while critical speculative work is suppressed;
- a 99 ms critical tick is throttled and does not sample again;
- 20% memory headroom restores normal pressure and hidden background policy;
- close removes runtime, pool, and controller state;
- a closed identity can be reused.
