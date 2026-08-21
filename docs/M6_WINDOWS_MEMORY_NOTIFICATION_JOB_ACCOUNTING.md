# M6 Windows low-memory notification and job accounting

This slice extends the existing Windows memory snapshot without adding a polling
thread or changing the process-level sampling cadence.

## Low-memory notification

The process owns one `LowMemoryResourceNotification` handle created with
`CreateMemoryResourceNotification`. Each existing sampler tick queries it with
`QueryMemoryResourceNotification`, which is non-blocking. A signaled low-memory
notification raises the process policy directly to `Critical`.

If notification creation or querying is unavailable, the snapshot keeps the
existing `GlobalMemoryStatusEx` memory-headroom behavior.

## Job-object accounting

When the process belongs to a Windows job, the sampler reads
`JobObjectExtendedLimitInformation` for the immediate job associated with the
current process. The snapshot records:

- enforced process-memory limit, when `JOB_OBJECT_LIMIT_PROCESS_MEMORY` is set;
- enforced job-memory limit, when `JOB_OBJECT_LIMIT_JOB_MEMORY` is set;
- private commit (`PROCESS_MEMORY_COUNTERS_EX::PrivateUsage`);
- peak process-memory usage;
- peak job-memory usage.

An enforced per-process memory limit narrows the effective pressure scope to:

- total = `min(host_total, process_memory_limit)`
- available = `min(host_available, process_memory_limit - private_commit)`

This is safe because `PrivateUsage` is current process commit and the process
limit is directly enforced on that process.

The job-wide limit and `PeakJobMemoryUsed` are accounting/telemetry only in this
slice. Peak job memory is historical rather than live current usage, so it is
not treated as remaining job headroom. Doing so would cause stale pressure after
a transient peak.

## Failure behavior

Job membership and job information are optional signals. Failure to resolve them
does not fail an otherwise valid Windows memory snapshot. The pre-existing
`GlobalMemoryStatusEx` and process working-set measurement remain the fallback.

The Windows API branch has a strict `_WIN32` syntax harness, and the platform-
independent process-limit arithmetic has focused regression coverage. A real
MSVC/Windows CI result remains separate evidence.
