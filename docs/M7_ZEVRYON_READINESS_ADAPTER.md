# M7 Zevryon readiness adapter

## Scope

`scripts/m7_zevryon_adapter.py` is the first concrete engine implementation of the vendor-independent M7 adapter protocol. It is intentionally fail-closed: it reports a successful competitor run only after all nine canonical metrics have real measurement primitives.

## Current admitted primitive

The adapter reuses `zevryon-zenith-frame-probe` for scroll timing. It passes the canonical workload's sample count, warmup count, viewport, overscan, maximum fragment count and scroll step directly to the native probe.

The native probe measures complete `ZenithTabRuntime::layout()` calls. The adapter validates the probe operation, selected device profile, warmup count, retained sample count and visible-layout count before reading the raw millisecond sample file.

`scroll_p99_ms` uses nearest-rank P99. `maximum_normal_stall_ms` is the maximum retained post-warmup sample. The device profile is selected from the RAM recorded in campaign `system_state`; the adapter deliberately ignores `ZEVRYON_DEVICE_PROFILE` so an environment override cannot change benchmark classification.

## Readiness behavior

At this stage only two canonical metrics have an admitted primitive:

- `scroll_p99_ms`
- `maximum_normal_stall_ms`

The other seven remain explicit readiness debt. The adapter therefore returns a valid failed raw-run with an empty metric object and a bounded `failure_mode` that lists missing metrics and the two measured frame values. This preserves the attempt in campaign evidence and prevents leadership claims.

The remaining metrics are not synthesized from performance targets or unrelated historical certification artifacts.

## Important semantic blockers

- A new `StoreReader` does not by itself establish OS-cold exact search; canonical cold search requires a fresh process per trial and later campaign-level cache-state control.
- Current arena mutation APIs publish persistent metadata. They must not mutate the canonical benchmark store. Mutation measurement requires an isolated scratch fixture or a reversible user-visible edit path.
- Preindexed and streaming first-viewport metrics need explicit native boundaries. Streaming must begin at raw corpus import and stop inside the progressive preview callback when a usable viewport is actually produced.
- Process-group PSS must be sampled across the benchmark process tree rather than copied from a profile target.

## Test

`m7-zevryon-readiness-adapter-smoke` injects a fake native frame probe, verifies campaign-RAM device selection, validates 1000 retained samples, nearest-rank P99 and maximum-stall semantics, and asserts that exactly seven metrics remain unavailable.
