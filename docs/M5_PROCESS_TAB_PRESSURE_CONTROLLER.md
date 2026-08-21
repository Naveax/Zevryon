# M5 — Process Tab Pressure Controller

## Purpose

This slice adds a process-level tab policy that is independent of tab count. Zevryon does not decide behavior from constants such as 100, 256, 1000, or 10000 tabs. Instead it coordinates every registered tab from visibility, current scroll activity, and process memory pressure.

The registry itself has no finite controller cardinality limit. Physical allocation failure remains possible on finite hardware, but there is no product tab-count wall.

## Runtime integration

`ZenithProcessTabController` stores one lightweight policy entry per registered tab and an activity sink. `make_zenith_tab_runtime_activity_sink()` binds that policy directly to `ZenithTabRuntime::set_activity()`.

The controller owns no rendering state, source cache, worker thread, StoreReader, or speculative payload. Those remain in their existing bounded authorities.

## Pressure policy

Global pressure has three states shared with the frame scheduler:

- `Normal`
- `Elevated`
- `Critical`

When pressure changes, hidden tabs are updated before visible tabs. This intentionally releases hidden-tab working sets before foreground tabs are touched, giving visible content first claim on memory.

For hidden tabs, scroll velocity is always normalized to zero before the sink is called. Through `ZenithTabRuntime`, this invalidates speculative motion authority, disables shared-prefetch activity, and applies background or critical hot-scroll trimming.

For visible tabs, the current scroll velocity is preserved. Critical pressure is still forwarded so the runtime can suppress speculative prefetch without discarding the foreground layout path.

Repeating the same global pressure value is idempotent and does not retrim every tab again.

## Failure behavior

A failing tab sink does not stop the process-wide pressure pass. The controller keeps applying the requested pressure to all remaining tabs, records failure telemetry, preserves the first diagnostic, and returns failure to the caller after the pass.

This is fail-forward pressure handling: one broken tab must not prevent other hidden tabs from releasing memory during a critical event.

## Resource scaling

The controller intentionally contains no `max_tabs` or equivalent finite threshold. Expensive resources remain bounded elsewhere:

- shared prefetch worker count is fixed independently of registered tabs;
- shared ready-result bytes are globally bounded;
- hidden tabs cannot perform frame work;
- hidden transitions release source-window and scratch working sets;
- critical hidden transitions also release checkpoint state.

Only lightweight policy metadata scales with the number of registered tabs, which is unavoidable if the browser must remember those tabs at all.

## Regression coverage

The focused controller test verifies:

1. a 4096-tab regression sample registers without a controller cardinality limit;
2. hidden registrations are normalized to zero scroll velocity;
3. critical pressure applies to hidden tabs before visible tabs;
4. visible velocity survives the process-pressure pass;
5. identical repeated pressure is idempotent;
6. hidden-to-visible activity inherits the current global pressure;
7. one injected sink failure does not prevent remaining tabs from receiving pressure policy.

The number 4096 is only a practical CI sample above the old 256-session default. It is not a Zevryon tab limit.

## Validation note

The controller implementation and focused test were also compiled outside the repository with C++20 and `-Wall -Wextra -Wpedantic -Werror`; the focused behavior test passed. Repository Windows/Linux CI remains the authority for full integration with the real `ZenithTabRuntime` and project build graph.
