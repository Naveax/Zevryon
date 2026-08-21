# M5 — Dormant Registry Scale Regression

The process-owner layer now has an explicit large hidden-tab regression that exercises dormant slots without touching a real store.

The test opens 4096 hidden tabs against a deliberately nonexistent store path. Because hidden registration is metadata-only, every unique tab must register successfully without constructing `ZenithTabRuntime`, opening a prefetch-pool session, starting workers, or retaining speculative payload.

The number 4096 is a regression sample, not a product limit.

The regression also deliberately transitions one dormant slot to Visible. Materialization must fail because the store does not exist. After that failure:

- materialized runtime count remains zero;
- shared-pool session count remains zero;
- the process controller is restored to the prior Hidden state;
- a second Visible attempt really retries and fails again rather than being incorrectly treated as an already-applied activity update.

Finally all 4096 slots close and process-owner, controller, and pool accounting must return to zero.
