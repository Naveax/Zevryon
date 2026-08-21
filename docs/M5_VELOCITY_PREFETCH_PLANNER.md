# M5 — Bounded Velocity Prefetch Planner

## Purpose

M5 requires velocity-aware prefetch without allowing speculative work to scale with tab count. This slice adds a deterministic policy that converts signed scroll velocity into a bounded source-window lead distance.

The planner does not create threads, queue slots, cache entries, or per-tab worker pools. It only chooses how far ahead the existing single-session speculative request should target.

## Default policy

The existing slow-scroll behavior is preserved:

- stationary: no speculative lead;
- slow: 1 window total, no additional lead;
- medium: 2 windows total, one additional window of lead;
- fast: 4 windows total, three additional windows of lead.

Default velocity thresholds are 128 CSS px/s and 512 CSS px/s expressed in Q8 units. Direction is intentionally not part of the tier decision: equal forward and reverse magnitudes receive the same lookahead tier, while the runtime applies the sign when choosing the source offset.

## Hard bounds

`VelocityPrefetchPolicy` requires monotonically increasing lookahead tiers and a hard `max_lookahead_windows` cap. The policy currently rejects caps above 64 windows. This is a prediction-distance bound, not a memory-retention count.

Lead-byte arithmetic saturates instead of overflowing, including for extreme input such as `INT64_MIN` velocity and very large window sizes. Invalid policy, stationary motion, and zero-sized windows fail closed to a stationary decision.

## Resource invariant

Velocity does not change:

- shared worker count;
- one-pending/one-ready-per-session pool semantics;
- global ready-result byte budget;
- hidden-tab suppression;
- process pressure policy;
- exact hot-cache admission correctness.

The process may have an unbounded number of registered tabs at policy level while expensive speculative execution remains globally bounded.

## Certification

The focused test covers:

- policy validation;
- stationary suppression;
- forward/reverse symmetry;
- exact medium and fast thresholds;
- preservation of the existing one-window slow behavior;
- `INT64_MIN` magnitude handling;
- saturating lead arithmetic;
- invalid-input fail-closed behavior.

Runtime source-offset integration is intentionally a separate child slice so planner correctness and runtime scheduling effects can be reviewed independently.
