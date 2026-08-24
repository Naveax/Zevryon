# M6 Linux cgroup v2 and PSI memory context

This slice starts the Linux half of M6 without changing M5 admission authority.

## Contract

- `/proc/meminfo` and `/proc/self/statm` remain the host/procfs fallback.
- On cgroup v2, `memory.max` and `memory.current` are read through bounded control-file reads.
- A finite cgroup limit only becomes the effective memory domain when it is smaller than host physical memory.
- Effective available memory is the minimum of host `MemAvailable` and `memory.max - memory.current`, clamped to zero when current usage is at or above the limit.
- `memory.max = max` keeps the host domain.
- cgroup memory PSI is preferred when available; host `/proc/pressure/memory` is the fallback.
- PSI `some avg10` and `full avg10` are stored as fixed-point milli-percent values, avoiding floating-point policy state.
- All optional cgroup/PSI discovery fails back to the existing procfs snapshot rather than making process-memory sampling unavailable.

## Snapshot authority

`ZenithProcessMemorySnapshot` now records:

- effective memory domain (`Host` or `CgroupV2`),
- whether cgroup v2 was detected,
- whether a cgroup limit is actually constraining the process,
- whether PSI was available,
- PSI `some avg10` and `full avg10` in milli-percent.

The existing M5 pressure policy automatically benefits from a constraining cgroup because its available/total ratio is evaluated against the effective memory domain rather than blindly against host RAM.

## Boundedness

- cgroup scalar control reads are capped at 256 bytes,
- PSI reads are capped at 16 KiB,
- no state grows with document or tab count,
- no new worker or polling loop is introduced by this slice.

## Tests

`zenith-linux-memory-context-tests` covers:

- finite and unlimited cgroup v2 parsing,
- fixed-point PSI parsing and range rejection,
- effective cgroup total/available calculation,
- non-constraining cgroup fallback to host memory,
- over-limit cgroup usage clamping available memory to zero.

## Deliberate non-credit

This slice does not yet use PSI thresholds to escalate `FramePressure`, does not add Linux cgroup event-driven notifications, and does not implement the Windows job-object/memory-notification half of M6. It earns only Linux effective-memory-domain and PSI-capture groundwork after exact-head CI.