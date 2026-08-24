# M5 — Shared Prefetch Pool O(1) Status

## Problem

`SharedSourcePrefetchPool::status()` previously walked the entire session registry to count active, queued, running and ready sessions. With an intentionally policy-unbounded tab registry, polling telemetry therefore cost O(number of registered sessions).

A dashboard or pressure controller asking for status should not turn a large tab count into repeated whole-registry scans.

## Change

The pool now maintains four lifecycle counters incrementally under the existing mutex:

- active sessions;
- queued sessions;
- running sessions;
- ready results.

`status()` copies those counters directly. Its work is now O(1) with respect to tab/session cardinality.

The counters are updated on registration, activation/deactivation, enqueue/dequeue, worker start/finish, ready publication/removal, close and stop. A small `counted` guard prevents a session that is closed while work is still completing from decrementing a process counter twice.

`wait_idle_for()` intentionally retains its registry scan because it is a coordination/test path and must verify pending state, whereas `status()` is the telemetry hot path.

## Validation

The focused regression uses 4096 registered sessions as a sample, not a product limit. It verifies:

- 4096 registered / 2048 active initial state;
- inactive-to-active transition updates the active counter;
- request execution returns queued/running to zero;
- ready publication increments exactly once;
- taking the result decrements exactly once;
- active session closes update both session and active counters.

The candidate implementation and focused counter harness were compiled locally with C++20 and strict warning-as-error flags before publication. The initial harness exposed an active-close ordering bug; that bug was fixed before the published candidate.
