# M5 — Hidden-Tab Frame Suppression

## Purpose

This stacked M5 slice makes tab visibility an explicit scheduler authority instead of relying on callers to remember not to enqueue work for hidden pages.

It builds on the existing frame-budget scheduler and bounded source-window prefetch worker. The parent already enforces visible-first admission, per-class budgets, worker-only blocking I/O, pressure throttling, and stale-prefetch epoch rejection. This slice adds the missing multi-tab rule: a hidden page receives no frame execution budget.

## Hidden-page contract

`FrameBudgetScheduler::begin_frame()` now accepts `FrameVisibility` in addition to pressure state. The default remains `Visible`, so existing callers preserve behavior.

For a hidden frame:

- Visible work is rejected with `SuppressedByVisibility`.
- Prefetch work is rejected with `SuppressedByVisibility`.
- Background work is rejected with `SuppressedByVisibility`.
- Maintenance work is rejected with `SuppressedByVisibility`.
- No work contributes to `spent_us`.
- The snapshot reports `remaining_us = 0`, so downstream code cannot mistake an unused hidden-page frame for spare CPU capacity.
- Entering hidden state neutralizes scroll direction and advances the prefetch epoch when motion was active. A prefetch worker that follows the scheduler authority ticket can therefore invalidate pending/ready speculative work rather than publishing it after the tab disappears.

The scheduler records cumulative hidden-frame and hidden-rejection counters so future Live100 evidence can distinguish deliberate hidden suppression from simple inactivity.

## Resume behavior

Returning to `Visible` does not retain the old speculative ticket. A fresh motion update creates a new epoch before new prefetch is admitted. Visible frame work can then resume normally.

This deliberately keeps visibility separate from memory pressure:

- visibility controls whether a page may consume frame CPU at all;
- pressure controls optional work budgets for a visible page;
- the separate hot-scroll memory-trim primitive controls how much cached/scratch memory an inactive page retains.

Combining those layers later allows a background tab to keep immutable document state without continuing to spend frame time or retain expensive source windows.

## Test coverage

The existing frame-budget scheduler test target now verifies:

- hidden transition invalidates active scroll-prefetch epoch;
- all four work classes are rejected while hidden;
- hidden frames consume zero budget and expose zero spendable remainder;
- hidden counters are cumulative and exact;
- returning visible permits visible work again;
- resumed scrolling receives a fresh prefetch epoch.

## Boundary

This is not a claim of complete tab suspension or Z11 Live100 completion. JavaScript timers, network activity, audio/media exemptions, service workers, compositor surfaces, process priority, and OS-level suspension require their own admitted subsystem contracts. The frame scheduler only guarantees that hidden pages cannot consume the frame-work classes it owns.
