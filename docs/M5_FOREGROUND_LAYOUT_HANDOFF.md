# M5 — Bounded Foreground Layout Handoff

## Purpose

The UI-lane blocking fence prevents the current synchronous hot-scroll path from pretending to be UI-safe. The next required primitive is a bounded ownership boundary that lets UI code publish the newest viewport request while worker-side code performs the existing synchronous layout work.

`SharedForegroundLayoutHandoff` provides that boundary without claiming a worker executor yet.

## Per-session authority

Each session retains at most:

- one pending request;
- one running request;
- one ready result.

Request identities are caller-owned, non-zero and strictly increasing. Reusing one identity with different request parameters fails closed. An identical duplicate coalesces.

A newer request invalidates an older pending request and any retained ready result. An older running request may finish physically, but publication is rejected when its identity is no longer current.

Activity transitions are also authority boundaries. Deactivating a session invalidates pending and ready work and marks any already-running request invalid. Reactivating the session does not resurrect that result.

## Memory bound

Ready-result retention is bounded process-wide by `max_ready_bytes`. Charge includes the ready envelope, retained fragment capacity and error-string capacity. A result that cannot fit the remaining budget is dropped instead of exceeding the bound.

`max_fragments_per_request` also rejects unbounded request envelopes before they enter the mailbox.

The handoff owns no worker threads in this slice. It therefore does not multiply thread resources with tab count.

## Concurrency contract

All mailbox state is serialized behind one process-level mutex. `try_take_pending()` permits at most one running request per session. A newer request may become pending while the older request is running, but it cannot run concurrently in that same session.

`publish_ready()` requires all of these simultaneously:

1. the session still exists;
2. the published identity matches the running request;
3. the running request was not invalidated by an activity transition;
4. the session is active;
5. the identity is still the newest request authority;
6. the ready payload fits the global memory budget.

Otherwise publication fails closed.

## Focused validation

The deterministic tests cover:

- duplicate coalescing;
- same-identity/different-payload rejection;
- latest-pending replacement;
- one-running-request-per-session;
- stale result rejection after a newer viewport request;
- ready-result delivery exactly once;
- hidden/inactive invalidation;
- the hide → show race where pre-hide running work must not become authoritative again;
- global ready-memory budget rejection;
- stop and session-close behavior.

## Admission boundary

This is **handoff-core credit only**. It does not yet make foreground layout asynchronous in production and does not close M5.

The next slice must connect a bounded worker executor to this mailbox and expose a UI-side request/poll API. Only after that integration can physical evidence test whether visible layout stays free of synchronous checkpoint/source I/O while preserving exact rendering correctness.
