# M6 Linux cgroup v2 and PSI Memory Pressure

This slice makes the process memory-pressure sampler aware of Linux cgroup v2
limits and Pressure Stall Information while preserving `/proc` as the fallback.

## Effective memory scope

The Linux sampler still reads host `MemTotal` and `MemAvailable`, but when the
current cgroup exposes finite `memory.max` and `memory.current` values the
snapshot is clamped to:

- total = `min(host_total, memory.max)`
- available = `min(host_available, memory.max - memory.current)`

An unlimited `memory.max=max`, missing cgroup files, or malformed optional
cgroup metadata leaves the existing host `/proc/meminfo` scope unchanged.

The cgroup path comes from the unified `0::/...` entry in `/proc/self/cgroup`.
Traversal components are rejected before resolving under `/sys/fs/cgroup`.

## PSI awareness

The sampler prefers cgroup-local `memory.pressure` and falls back to
`/proc/pressure/memory`. `avg10` values are parsed without floating-point or
locale dependence and stored as Q16 fractions of 100 percent.

Default policy thresholds are:

- `some avg10 >= 10%` enters `Elevated`
- `full avg10 >= 2%` enters `Critical`
- 1% PSI recovery hysteresis prevents immediate state flapping

Memory-headroom thresholds and their existing hysteresis remain authoritative
in parallel; PSI can only raise pressure, not hide low-memory pressure.

## Failure behavior

Cgroup and PSI discovery are optional. Failure to read or parse them does not
fail the memory snapshot if `/proc/meminfo` and `/proc/self/statm` remain valid.

No new polling thread is introduced. The existing process-level adaptive sampler
cadence continues to own when OS memory signals are captured.
