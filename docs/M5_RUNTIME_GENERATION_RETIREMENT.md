# M5 — Runtime Generation Retirement

## Goal

Process-level tab close and critical hidden dematerialization must not wait for an already-running foreground layout callback.

## Runtime generation identity

Public tab/session identity is now separate from the process-internal `ZenithTabRuntime` session identity used by the shared foreground-layout and source-prefetch pools.

Each materialization receives a monotonically increasing non-zero runtime generation id. A later materialization of the same public tab receives a fresh generation id, so an older retired callback cannot collide with or publish into the new runtime's pool authority.

Generation ids are never intentionally reused. Exhaustion fails closed.

## Retirement

Before retirement, the runtime is transitioned to hidden/critical authority. This invalidates pending/ready foreground work and speculative source-prefetch authority while requesting hot-scroll critical trimming without waiting for the active layout lock.

The process owner then moves the runtime into a retired-generation list instead of destroying it synchronously. Public tab state can therefore become dormant or close immediately while any already-running callback retains the old runtime lifetime it needs.

Retired generations are destroyed when the process foreground worker pool reports no running callbacks. Destruction then closes the old internal pool sessions without waiting on foreground execution.

## External identity reuse

Because pool identity is generation-scoped rather than public-tab-scoped, a public session id may be reused or rematerialized while an older generation is still retired. The new runtime receives independent foreground/source authority.

## Resource accounting

`ZenithProcessRuntimeServicesStatus::retired_runtime_generations` exposes retained old runtime generations. They are not counted as materialized tabs. Their heavy cache state has already received hidden/critical trim authority before retirement.

## Boundary

Retirement is process-lifecycle ownership. It does not change the foreground mailbox latest-request rule, physical frame certification requirements, or source correctness invariants.
