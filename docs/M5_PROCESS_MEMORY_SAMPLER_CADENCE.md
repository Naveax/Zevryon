# M5 — Process Memory Sampler Cadence

## Purpose

The process memory-pressure policy now has a caller-driven sampling cadence suitable for a browser/process event loop. The sampler owns no background thread and is process-level rather than tab-level.

## Default cadence

- Normal pressure: 1000 ms
- Elevated pressure: 250 ms
- Critical pressure: 100 ms

The first poll samples immediately. Calls before the next due monotonic timestamp return `Throttled` without touching the operating-system snapshot provider.

After a snapshot changes pressure, the next interval is selected from the new pressure state. This makes low-pressure operation cheap while increasing observation frequency when memory headroom becomes dangerous.

## Failure behavior

Snapshot capture failures are fail-visible but are still rate-limited. A failed capture schedules the next attempt using the current pressure cadence instead of allowing an event loop to enter a tight operating-system polling loop.

The sampler exposes an injectable snapshot provider for deterministic tests and embedders, while production defaults to `capture_zenith_process_memory_snapshot()`.

## Scaling contract

The sampler is deliberately threadless and process-scoped:

- no thread per tab;
- no timer object per tab;
- no registry scan merely to decide whether memory should be sampled;
- pressure application still occurs only when the state actually changes because the existing controller bridge is idempotent.

Tab registry cardinality remains policy-unbounded. Sampling cost is bounded by process cadence, not by the number of registered tabs.

## Validation

The focused regression certifies:

- immediate first sample;
- Normal → Elevated → Critical adaptive intervals;
- early event-loop ticks are throttled without provider calls;
- recovery restores the Normal interval;
- capture failure is backed off rather than retried every tick;
- reset permits an immediate fresh sample;
- controller pressure applications occur only for actual state transitions.
