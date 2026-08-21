# M5 — Dormant Hidden Tab Slots

## Problem

A policy-unbounded tab registry is not useful if every hidden tab eagerly constructs a full `ZenithTabRuntime`, opens hot-scroll/store state, registers a prefetch session, or even occupies the process pressure controller's active working-set registry.

## Dormant slot contract

`ZenithProcessRuntimeServices` stores lightweight tab slots separately from materialized runtimes.

A tab opened as `Hidden`:

- retains only store root, device profile, layout contract, and lightweight activity metadata in the process owner;
- does not create `ZenithTabRuntime`;
- does not open a shared-prefetch pool session;
- does not start worker threads;
- does not open hot-scroll/store state;
- does not register an entry in `ZenithProcessTabController`.

The first transition to `Visible` registers the slot with the process controller. The controller sink materializes the runtime on demand and applies the controller's current global pressure before returning.

## Critical dematerialization

A materialized tab hidden under Normal/Elevated pressure may keep its already-open runtime after background trimming for fast return. It remains controller-relevant because a later Critical transition must reclaim it.

When global pressure becomes `Critical`, hidden materialized tabs are processed before visible tabs. The hidden runtime first receives Critical activity so caches are trimmed, then the process owner destroys that runtime. Its shared-prefetch session closes and its controller entry is removed after the pressure pass completes, returning the tab to a metadata-only dormant slot.

Visible runtimes are never dematerialized by this path. Foreground layout remains authoritative while Critical pressure suppresses speculative prefetch.

Recovery to Normal does not eagerly recreate dormant hidden tabs or register them in the controller. They re-enter the materialized/controller working set only when they become Visible again.

## Scaling and telemetry

`tabs` counts all process-owned slots. `materialized_tabs` is maintained incrementally and is O(1) to read.

The process controller now tracks only materialized runtime work. A large dormant registry therefore does not turn each pressure transition into a whole-history tab scan. Pressure cost follows the resource-bearing working set rather than total tab cardinality.

There is still no finite tab-count admission constant. Natural metadata allocation failure remains the physical limit.

## Activity failure recovery

A dormant slot that fails to materialize as Visible is not added to the controller. For already controller-managed slots, the process owner remembers the last successfully applied activity and reconstructs controller registration after a failed transition so a failure cannot become a false permanent success.

## Validation

The focused regressions prove:

- hidden open creates no runtime, pool session, worker, or controller entry;
- visible open materializes and enters the controller working set;
- Normal hide may retain the already-open runtime while deactivating speculative authority;
- a dormant tab materializes on visibility;
- Critical pressure dematerializes hidden runtime state and removes its controller entry while preserving visible runtime;
- visible foreground layout succeeds under Critical while speculative prefetch remains suppressed;
- Normal recovery does not eagerly recreate dormant hidden state;
- a 4096-hidden-tab regression sample has zero materialized runtimes, zero pool sessions, and zero controller entries;
- failed visible materialization leaks no runtime/controller state and remains retryable.

The number 4096 is a regression sample, not a product limit.
