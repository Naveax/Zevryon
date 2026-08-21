# M5 — Dormant Hidden Tab Slots

## Problem

A policy-unbounded tab registry is not useful if every hidden tab eagerly constructs a full `ZenithTabRuntime`, opens hot-scroll/store state, and registers a shared-prefetch session. That still makes native runtime state scale with every merely registered tab.

## Dormant slot contract

`ZenithProcessRuntimeServices` now stores lightweight tab slots separately from materialized runtimes.

A tab opened as `Hidden`:

- is registered with the process tab controller;
- retains store root, device profile, layout contract, and lightweight activity metadata;
- does not create `ZenithTabRuntime`;
- does not open a shared-prefetch pool session;
- does not start worker threads;
- does not open hot-scroll/store state.

The first transition to `Visible` materializes the runtime on demand and applies the controller's current global pressure before returning.

## Critical dematerialization

A materialized tab that is hidden under Normal/Elevated pressure may keep its already-open runtime after background trimming for fast return.

When global pressure becomes `Critical`, hidden tabs are processed before visible tabs. The hidden runtime first receives Critical activity so caches are trimmed, then the process owner destroys that runtime entirely. Its shared-prefetch session closes with it and the tab returns to a dormant metadata-only slot.

Visible runtimes are never dematerialized by this path. Their foreground layout remains authoritative while Critical pressure suppresses speculative prefetch.

Recovery to Normal does not eagerly recreate dormant hidden tabs. They rematerialize only when they become Visible again.

## Scaling and telemetry

`tabs` counts all registered slots. `materialized_tabs` is maintained incrementally and is O(1) to read; status does not scan the tab registry merely to count materialized runtimes.

There is still no finite tab-count admission constant. Natural metadata allocation failure remains the physical limit.

## Activity failure recovery

The process owner remembers the last successfully applied slot activity. If a controller activity transition fails, it reconstructs the controller registration using that last successful state so a failed materialization does not leave the controller believing a transition succeeded and suppressing a later retry.

## Validation

The focused regression opens one hidden and one visible tab over a real MassiveDoc fixture and proves:

- hidden open creates no runtime and no pool session;
- visible open materializes exactly one runtime;
- normal hide keeps the bounded already-open runtime but deactivates speculative authority;
- making the dormant tab visible materializes it;
- Critical pressure dematerializes the hidden runtime while preserving the visible one;
- visible foreground layout still succeeds under Critical and speculative prefetch stays suppressed;
- a throttled 99 ms process tick does not resample;
- Normal recovery does not eagerly rematerialize the hidden slot;
- making it visible again recreates its runtime;
- closing all tabs releases all materialized runtime and pool-session state.
