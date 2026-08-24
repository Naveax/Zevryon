# M5 — Dormant Registry Scale Regression

The process-owner layer has an explicit large hidden-tab regression that exercises dormant slots without touching a real store.

The test opens 4096 hidden tabs against a deliberately nonexistent store path. Because hidden registration is metadata-only, every unique tab must register successfully without constructing `ZenithTabRuntime`, opening a prefetch-pool session, starting workers, retaining speculative payload, or entering the materialized pressure-controller registry.

The number 4096 is a regression sample, not a product limit.

The regression deliberately transitions one dormant slot to Visible. Materialization must fail because the store does not exist. After that failure:

- materialized runtime count remains zero;
- shared-pool session count remains zero;
- process-controller registered count remains zero;
- a second Visible attempt really retries and fails again rather than being incorrectly treated as an already-applied transition.

Finally all 4096 slots close and process-owner, controller, and pool accounting return to zero.

This separates total tab history/cardinality from the resource-bearing working set. Memory-pressure transitions no longer need to visit dormant tabs that own nothing reclaimable.
