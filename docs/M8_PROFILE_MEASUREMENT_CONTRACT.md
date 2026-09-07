# M8 raw four-profile measurement contract

## Purpose

The existing `m8_profile_observation_gate.py` is the no-compensation evaluator for `zevryon.m8.profile-observations.v1`. It intentionally does not manufacture observations.

Physical certification cannot collect all four named device classes in one process on one host. The frozen collection boundary is therefore two-stage:

1. each qualifying physical environment produces one create-only `zevryon.m8.profile-case.v1` artifact with authority `m8-raw-profile-case-v1`;
2. `m8_profile_collection_binder.py` consumes exactly four distinct cases plus one canonical certification-mode Titan report and creates:
   - `profile-observations.json`, exact schema `zevryon.m8.profile-observations.v1`;
   - `profile-collection-receipt.json`, schema `zevryon.m8.profile-collection.v1`, authority `m8-four-profile-collection-binder-v1`.

The observations file stays verdict-free so the admitted evaluator recomputes `score_100`. The collection receipt preserves case hashes and the raw-to-observation recomputation chain needed to prove that the values were measured rather than typed by hand.

Final bundle admission must hash and validate the Titan report, four profile cases, observations artifact and collection receipt. An observations JSON without its matching collection/case evidence is not admissible final M8 evidence.

## Candidate and Titan binding

Every per-profile case binds:

- exact candidate commit and tree;
- exact collector executable SHA-256 in the collector's phase receipt;
- exact canonical Titan fixture report SHA-256;
- exact Titan ZMDOC container and logical-payload SHA-256;
- Titan report candidate commit/tree equal to the case candidate;
- the exact frozen Titan envelope and special-record receipts.

The collection binder rejects smoke Titan evidence for physical profile certification.

All four cases for one final decision must refer to the same candidate and same Titan payload identity. Cross-candidate or cross-Titan mixing is invalid evidence.

## Profile runtime policy

C++ production-facing profile constants are defined in `massivedoc_profile_policy.hpp`. CI compares every one of them against `zevryon_platform.performance_contract.DEVICE_PROFILES`; drift is a failure.

The profile hot/warm/cold numbers are upper cache budgets, not a requirement to consume every byte. The runtime policy never invents a cache consumer merely to fill a budget.

### Hot budget

- the admitted immutable-block hot cache keeps its existing bounded default reservation;
- all remaining hot budget is the ceiling for `LayoutWindowEngine`'s measured-layout LRU;
- hot accounting must sum to no more than the profile hot budget.

### Warm budget

- the admitted immutable-block warm cache keeps its existing bounded default reservation;
- the existing persistent-checkpoint cache receives at most its existing default ceiling;
- the existing source-window cache receives at most its existing default ceiling;
- unused warm budget is recorded as `warm_unallocated_bytes` and is not silently reassigned.

### Cold budget

The store cold mapped window receives at most the profile cold budget and never exceeds the current process address-space mapped-window hard limit. Any remainder is recorded as `cold_unallocated_bytes`.

A 32-bit process may intentionally use less cold mapped residency than the same named profile on 64-bit. Lower cache residency does not relax any latency gate.

## Physical profile rule

A case may only be labelled as a device class when the host satisfies the frozen physical-memory minimum and the case records the physical-host evidence required by the M0/M7 physical-host authority.

Environment-variable profile forcing is useful for CI smoke and policy tests but is not physical certification. A desktop run with `ZEVRYON_DEVICE_PROFILE=legacy-phone` does not become legacy-phone physical evidence.

A final four-profile collection therefore requires four qualifying physical environments or four separately constrained/attested environments whose authority explicitly satisfies the physical-profile rule. No desktop emulation is accepted merely because a memory cap was configured.

## Memory metric

`process_group_pss_mb` means absolute aggregate proportional set size in decimal MB. File-backed resident pages count. Empty-process baseline subtraction is forbidden.

On Linux the authority is `/proc/<pid>/smaps_rollup` PSS summed across the exact owned process group. The case records every PID and sample. If the metric probe is verified to own no child processes, the one probe PID is the complete process group and that fact is recorded.

RSS, Windows working set, private bytes, commit charge or a generic resident-process counter may not be relabelled as PSS. A platform without an admitted PSS-equivalent authority may run collector smoke but may not emit final profile certification evidence.

The observation uses the maximum aggregate PSS seen from the start of measured profile work through completion of all measured operations, not the most convenient individual phase.

## First viewport: streaming

`first_viewport_streaming_ms` measures a real progressive-import boundary.

The timer begins immediately before `import_zmdoc_corpus_progressive()` starts consuming the canonical Titan ZMDOC.

The first progressive-preview callback is eligible only after the production writer has produced a readable prefix snapshot and compact preview arena. Inside that callback the collector constructs the production layout engine on the preview store, opens it and executes a usable first viewport at scroll position zero.

The timer stops only after that layout succeeds with nonempty rendered output. Full import must still have remaining records at this point. The case records total records, remaining records, preview store/arena statistics and the first-layout receipt.

A viewport produced after full import is not streaming-first-viewport evidence.

## First viewport: preindexed

`first_viewport_preindexed_ms` is measured after the full canonical Titan store and compact arena are complete.

A fresh production layout engine is constructed with the frozen profile runtime policy. The timer begins before engine open and stops after the first successful layout at scroll position zero. The receipt includes the layout result and verifies the full Titan store identity.

This metric does not include corpus generation or import time. It measures first usable viewport from an already prepared full store.

## Scroll P99 and normal-stall maximum

One persistent production `LayoutWindowEngine` remains open after the preindexed first viewport.

The collector performs:

- 16 deterministic warmup layouts, excluded from statistics;
- 257 deterministic measured scroll layouts across the complete logical height.

Coordinates are generated from a frozen deterministic generator and are recorded in order. Each call is timed with `std::chrono::steady_clock` around only the production `layout()` operation.

The collection binder recomputes `scroll_p99_ms` from all 257 raw samples using linear percentile interpolation at 99%, position `(N - 1) * 0.99` in sorted order.

`maximum_normal_stall_ms` is the maximum of the same 257 measured normal-scroll samples. A timeout, layout error, truncated-invalid receipt or missing sample is a collector failure, not a zero-millisecond sample.

## Exact search cold and warm

The deterministic exact query is the frozen Titan tail marker `ZEVRYON_M8_TITAN_TAIL`, which occurs in the final ordinary record.

The words `cold` and `warm` are frozen as application-cache state, not privileged operating-system page-cache state:

- cold exact search: construct a fresh production `StoreReader` with the profile runtime policy, open it, explicitly evict the immutable block cache to cold, then time the first exact `find()` for the tail marker;
- warm exact search: immediately repeat the exact same `find()` on the same reader and time only that call.

Both calls must return the same exact terminal hit identity. The case records block-cache statistics before/after each query and the host filesystem/cache-state metadata available to the collector.

The collector does not claim the kernel page cache was flushed. Privileged whole-host cache flushing is deliberately excluded because it is destructive, platform-specific and not reproducible across the four physical profiles.

## Mutation P95

Mutation timing uses the production persistent compact arena.

The collector selects deterministic valid record indices from the full Titan logical sequence and performs 257 measured `CompactArenaReader.update_height()` operations after 16 warmups. Each update toggles between two frozen valid heights so the operation is real and persistent rather than a no-op.

Every returned old/new height and total-height receipt is checked. The collection binder recomputes `mutation_p95_us` from all 257 raw samples using linear interpolation at 95%, position `(N - 1) * 0.95`.

The collector restores the original logical height state before the phase is considered complete and verifies reopen consistency.

## Copy throughput

Copy throughput uses the production full-document selection/export path, not a memory-to-memory surrogate.

The collector obtains `full_document_selection(arena.logical_snapshot())` and runs `export_full_document()` in text mode to a create-only external output file. Timing covers the production export call. The case requires:

- selected text bytes equal the canonical Titan logical UTF-8 byte count;
- exported source bytes equal that count;
- output bytes equal that count;
- bounded-buffer statistics remain valid;
- output SHA-256 equals the Titan logical-payload SHA-256;
- cancellation is false.

The collection binder recomputes:

`copy_throughput_mib_s = source_bytes / 1,048,576 / elapsed_seconds`.

## Correctness event counters

`data_loss_events`, `invalid_utf8_events` and `crashes_or_ooms` are not hand-entered zeros.

The per-profile case derives/preserves them from phase outcomes and explicit postconditions. The collection binder additionally forces a data-loss event when copy byte counts/hash/cancellation disagree with Titan identity, forces an invalid-UTF8 event when exported output is reported invalid, and forces a crash/OOM event when the probe terminated abnormally or returned nonzero.

A process that dies before its child receipt can be completed is handled by the parent collector and cannot be interpreted as zero crash events.

## Raw per-profile case requirements

Each case preserves at least:

- physical-host/profile qualification receipt and SHA-256;
- profile runtime-policy/budget receipt;
- exact process/PID ownership receipt;
- all PSS samples;
- streaming-first-viewport timestamps and progressive-preview receipt;
- preindexed first-viewport receipt;
- all scroll warmup/measured coordinates and raw times;
- cold/warm exact-search raw timings, hit identities and cache statistics;
- all mutation indices, old/new receipts and raw timings;
- copy/export timing, byte counts, bounded-buffer stats and output SHA-256;
- derived correctness-event sources;
- child probe return/termination receipt.

The collection binder does not accept a per-case observation summary. It constructs the final observation itself from raw fields plus the canonical Titan envelope.

## Single-use and no cherry-picking

A per-profile collection attempt is single-use for its exact candidate + Titan + physical environment. A valid failing result is preserved. Repeating an identical case until it passes and selecting the preferred run is not admissible.

The final four-profile collection consumes exactly one case for each device class. No metric may be copied from another case or bundle.

## CI smoke boundary

CI may exercise the runtime policy, collection binder and a reduced collector fixture. CI smoke is explicitly nonphysical and noncertifying. It may not create a four-profile physical `score_100` claim by substituting target constants for measurements.

## Final bundle dependency

Before final M8 bundle execution, `m8_bundle_import_profile.py` and the final binder must be hardened so profile import requires:

- the canonical Titan report;
- all four per-profile case artifacts;
- the generated `profile-observations.v1` artifact;
- the matching `profile-collection.v1` receipt;
- successful raw recomputation and artifact SHA-256 checks.

Until that admission exists, the production certification runbook remains blocked from freezing a final M8 bundle.
