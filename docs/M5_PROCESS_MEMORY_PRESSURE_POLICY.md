# M5 — Process Memory Pressure Policy

## Purpose

The process tab controller already knows how to apply `Normal`, `Elevated`, and `Critical` pressure to tabs, but it previously required an external caller to invent that pressure value. This layer supplies a deterministic process-wide memory signal without using tab count as a proxy for memory pressure.

## Policy

The default state machine uses available physical-memory ratio in Q16 form:

- enter `Elevated` at or below 15% available;
- enter `Critical` at or below 8% available;
- require an additional 3% headroom before recovering from the current pressure state.

The hysteresis prevents repeated Normal/Elevated/Critical flapping when available memory hovers around one threshold. Critical pressure can still be entered immediately from any state.

`process_rss_bytes` is captured for telemetry but is not used by the initial decision policy. This keeps the first authority based on actual host memory headroom rather than a product-specific per-process target or tab cardinality.

## Platform snapshot

Windows uses `GlobalMemoryStatusEx` for total/available physical memory and `GetProcessMemoryInfo` for process working set. Linux uses `/proc/meminfo` (`MemTotal`, `MemAvailable`) and `/proc/self/statm` for resident pages. Unsupported platforms fail closed.

## Controller bridge

`sample_and_apply_zenith_process_memory_pressure()` captures one snapshot, updates the stateful policy, and calls `ZenithProcessTabController::set_global_pressure()` only when the resulting pressure differs from the controller's current pressure. Therefore repeated samples in the same state do not trigger whole-registry pressure application.

Actual pressure transitions retain the controller's existing hidden-first semantics, so hidden tabs are reclaimed before visible tabs receive the new pressure state.

## Validation

The focused test verifies Normal/Elevated/Critical entry, both recovery hysteresis bands, invalid-snapshot rejection, and a real platform snapshot. The Linux implementation and policy were compiled locally with C++20 and `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror` before publication. Windows compilation and runtime behavior remain part of repository CI evidence.
