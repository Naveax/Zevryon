# M6 Windows low-memory and job-object context

This slice adds bounded Windows low-memory and job-object evidence to the existing process memory snapshot without changing default pressure policy.

## Baseline preserved

`GlobalMemoryStatusEx` remains the host physical-memory source and `GetProcessMemoryInfo` remains the process working-set source.

## Additional Windows context

When available, the snapshot records:

- `LowMemoryResourceNotification` state from `CreateMemoryResourceNotification` / `QueryMemoryResourceNotification`,
- whether the process belongs to a Windows job object,
- active process count from `JobObjectBasicAccountingInformation`,
- configured process and job memory limits from `JobObjectExtendedLimitInformation`,
- peak process and job memory usage from the same job information record.

The current process job is queried with a null job handle, which Windows defines as the job associated with the calling process; for nested jobs this is the immediate job.

## Fail-safe behavior

Optional Windows context discovery does not replace the existing host-memory fallback. Missing notification or job metadata leaves those fields unavailable rather than making the entire memory snapshot fail.

Limit metadata is rejected if a limit is marked enabled with a zero byte value. A low-memory signal is authoritative only when the notification query itself succeeded.

## Deliberate boundary

This slice captures Windows notification/job metadata only. It does not yet:

- convert the low-memory notification into a pressure floor,
- use job limits as an effective memory domain without trustworthy current job-memory usage,
- keep a persistent notification handle or event-driven watcher.

Those remain separate M6 policy/lifecycle slices.