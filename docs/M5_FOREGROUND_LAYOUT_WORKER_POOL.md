# M5 — Shared Foreground Layout Worker Pool

## Purpose

The bounded foreground handoff defines latest-request authority and ready-result memory limits. This slice adds the process-shared worker owner that can execute a session's synchronous layout callback away from the UI lane.

## Process resource bound

`SharedForegroundLayoutWorkerPool` owns a fixed worker count configured once for the process. The hard contract rejects zero workers and counts above 64. Workers are created when the first session is opened, not per tab.

Each session stores only its executor callback plus bounded scheduling metadata. Heavy result retention remains governed by `SharedForegroundLayoutHandoff::max_ready_bytes`.

## Scheduling

The worker pool delegates request identity, pending/running/ready authority and ready-memory accounting to `SharedForegroundLayoutHandoff`.

Pool scheduling adds these rules:

- a session is queued at most once;
- a session runs at most one foreground layout at a time;
- a newer request that arrives while a worker is already executing sets one rerun marker, not another unbounded queue entry;
- when the worker has not yet claimed the mailbox, it may claim the newer pending request directly and the redundant rerun is suppressed by comparing the claimed request id with the latest scheduled id;
- after an older request finishes, exactly one requeue occurs only when a strictly newer request is still pending.

## Worker publication

The executor callback receives an immutable `ForegroundLayoutRequest` and caller-owned output structures. The pool, not the executor, binds the ready envelope to the request id. The handoff then decides whether the completed result is still authoritative and fits the global ready-memory budget.

A physically successful execution may therefore be discarded when it became stale while running. Execution success and publication authority are intentionally separate facts.

## Activity and lifecycle

Deactivating a session first suppresses further pool scheduling and then invalidates mailbox authority. A running callback may finish, but its result cannot publish across the activity transition.

`close_session()` waits for an already-running callback to leave the executor before destroying the stored callback. This is a lifecycle safety boundary, not a frame-time operation.

`stop()` invalidates mailbox authority, wakes workers, joins the fixed worker set and clears session metadata.

## Focused validation

The worker-pool tests prove:

- no worker exists before the first session;
- exactly the configured process worker count starts once sessions are used;
- a newer request arriving during an older running layout produces only the newest ready result;
- no redundant empty rerun occurs when the worker already claimed the newest request;
- pre-hide running work cannot survive hide → show;
- resumed fresh work is deliverable;
- worker count above the hard process bound is rejected;
- stop prevents later session creation.

## Admission boundary

This is **worker-pool core credit**, not production UI integration. `ZenithTabRuntime` is not yet registered with this pool and no public UI request/poll bridge is wired in this slice.

The next slice must connect runtime ownership to this pool using the worker-lane synchronous layout compatibility path, preserve tab teardown safety, and prove UI-side request/poll operations do not enter checkpoint or source I/O.
