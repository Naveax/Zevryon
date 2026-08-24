# M5 — Current-Main Consolidation

## Baseline

This branch ports the admitted M5 scheduling/prefetch work onto current `main` (`027c8133b5e1ee64a1c1a2a0e6a7e9daa4fbb58a`) instead of extending the historical M5 branch whose merge base predates the current agent CI policy.

## Consolidated slices

The branch combines the following independent work without importing unrelated historical tree state:

1. deterministic frame-budget scheduling with visible-first admission, optional-class caps, UI blocking rejection, pressure suppression, scroll-direction prefetch epochs, and hidden-page zero-frame-budget suppression;
2. the compatibility source-window prefetch worker with bounded latest-pending/ready state and lazy native-thread startup;
3. the shared cross-tab source-prefetch pool with fixed worker count, bounded sessions, round-robin requeue fairness, hidden/inactive authority cancellation, and a global ready-result memory budget;
4. hot-scroll tab-pressure memory trimming, including background source/scratch release and critical checkpoint release while preserving immutable document authority.

## Why consolidate before deeper integration

The earlier M5 chain was functionally useful but history-diverged from current `main`. Adding production ownership wiring on top of that history would make later review and merge unnecessarily ambiguous. This consolidation makes the next integration work start from the exact current-main tree while retaining the current CI execution policy.

No historical commit is treated as authority merely because it exists. The files in this branch are explicit ports onto current main and must pass current Windows/Linux build and test gates on this exact head.

## Next production step

The next slice will connect `ZenithHotScrollSession`/tab ownership to a process-level `SharedSourcePrefetchPool` and the frame-budget authority. That integration must preserve the existing bounded synchronous fallback and hot-scroll certification behavior, so speculative prefetch can improve latency but can never become required for correctness.

## Boundaries

This consolidation does not claim M5 completion or Live100 completion. In particular, JavaScript timer throttling, network lifecycle, media exemptions, compositor scheduling, renderer process priority, service workers, and OS suspension remain separate future contracts.
