# M5 — Process Memory Pressure Runtime Integration

This slice locks the memory-headroom policy to real `ZenithTabRuntime` behavior rather than validating only enum transitions.

A deterministic `apply_zenith_process_memory_pressure_snapshot()` entry point accepts an already captured snapshot, updates the stateful hysteresis policy, and forwards pressure to `ZenithProcessTabController` only when the state changes. The platform-sampling entry point remains responsible for capturing Windows/Linux memory data; the deterministic path exists for integration tests and embedders that already own an authoritative memory snapshot.

The real-runtime integration test prepares a MassiveDoc store, compact arena, and layout checkpoint, then opens two tab runtimes on one shared prefetch pool:

- one hidden runtime;
- one visible runtime.

At 7% available system memory the default policy enters `Critical`. The test requires the hidden runtime to perform critical hot-scroll trimming and remain inactive for speculative prefetch, while the visible runtime retains foreground layout capability. Critical pressure must suppress visible speculative prefetch without suppressing visible rendering.

Applying the exact same critical snapshot again must not increment controller pressure-change telemetry or retrim the hidden runtime. Recovering to 20% available memory must restore `Normal` pressure and reapply the hidden background policy.

The controller's separate focused test continues to certify hidden-before-visible application order. This integration test certifies the concrete runtime consequences of that ordering.
