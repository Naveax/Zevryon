# M6 Windows process job-memory scope

This slice narrows the effective memory domain when Windows reports an enforced per-process job memory limit and the current process commit charge is known.

## Safe inputs

The calculation uses only values that describe the same process:

- `JOB_OBJECT_LIMIT_PROCESS_MEMORY` / `ProcessMemoryLimit`,
- `PROCESS_MEMORY_COUNTERS_EX::PrivateUsage`,
- host total and available physical memory.

The effective scope is:

- total = `min(host_total, process_limit)`,
- process headroom = `max(process_limit - private_commit, 0)`,
- available = `min(host_available, process_headroom, effective_total)`.

This makes the existing available-memory pressure policy conservative inside an enforced process limit without inventing memory above either host or job reality.

## Deliberate exclusions

The job-wide memory limit and `PeakJobMemoryUsed` remain telemetry only. Peak job usage is historical, not current usage, so subtracting it from a job-wide limit could leave pressure falsely elevated after a transient peak. A job-wide effective domain requires trustworthy live job consumption and is not claimed by this slice.

## Failure behavior

No process limit means the host scope is preserved unchanged. A limit marked enabled with a zero byte value is rejected. If Windows context discovery is unavailable, the existing host-memory fallback remains authoritative.

## Validation

The deterministic scope test covers unlimited behavior, a finite process limit, exhausted process headroom, zero-limit rejection and invalid host-memory input. Windows exact-head CI remains the admission authority for the real `PrivateUsage` capture path.
