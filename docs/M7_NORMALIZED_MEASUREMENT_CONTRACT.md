# M7 normalized measurement boundary contract

## Purpose

`docs/M7_CORE_METRIC_CONTRACT.md` fixes the five lower-is-better leadership metrics before evidence collection. This document fixes the timing and memory boundaries required to populate those metrics without comparing implementation-specific phases as though they were equivalent.

This contract is fail-closed. Existing fields may remain useful diagnostics, but a field is not leadership-admissible merely because it has the same unit or a similar name.

## Canonical case lifecycle

Every implementation must execute the same logical lifecycle:

1. harness baseline;
2. case start;
3. case-owned runtime/process launch;
4. source/corpus open or construction required by that implementation;
5. viewport/scenario initialization;
6. required index/checkpoint/layout preparation;
7. declared warmup queries, if any;
8. ready boundary;
9. deterministic measured query sequence;
10. final memory sample;
11. case teardown.

Corpus generation that is performed once outside every competitor case is excluded for every implementation. Any competitor-specific transformation required after case start is setup work and cannot be moved outside the boundary selectively.

## `setup_to_ready_seconds`

### Start boundary

The timer starts immediately before launching the case-owned runtime/process for the implementation.

The start boundary must therefore precede:

- browser/engine process launch;
- Zevryon persistent benchmark process launch;
- source/store open performed by the case-owned process;
- payload materialization performed after runtime launch;
- index/checkpoint creation required before measured queries;
- viewport/page/scenario initialization;
- declared warmups.

A shared control service that is deliberately outside every competitor case may remain outside the timer only when the same exclusion is represented by the scenario/evidence authority. An implementation may not receive a private pre-launched runtime while another pays process-launch cost.

### Ready boundary

The timer stops only when the implementation can accept the first measured query without additional required preparation.

If warmup queries are declared, all warmups finish before the ready boundary and are therefore charged to setup. Warmup samples never enter P50/P95/P99.

The report must publish the exact boundary identifier:

`case-owned-runtime-launch-to-post-warmup-ready-v1`

## Query timing boundary

Measured query latency represents implementation-local query work, not harness IPC/automation transport overhead.

For browser/WebDriver/Playwright cases, the authoritative sample is the page-side elapsed time from query mutation/work start until the existing double-`requestAnimationFrame` completion boundary.

For Zevryon, the authoritative sample is measured inside one persistent case-owned process from query execution start until the bounded result is ready. Pipe/stdin/stdout transport, process creation, store/checkpoint open and JSON serialization are excluded from per-query samples because their corresponding browser automation transport is also excluded.

The persistent Zevryon path is the only normalized timing source. Legacy `checkpoint-window` process-per-query wall-clock timings remain diagnostics and are not normalized leadership evidence.

Every successful normalized case must publish the raw measured sample list and sample count. P50/P95/P99 are recomputed from that exact list by the normalized evidence authority.

## Warmup authority

`warmup_query_count` is part of the normalized scenario authority and must be identical for all competitors in a comparable group.

The warmup count must be fixed before canonical evidence collection. Changing it creates a new scenario fingerprint and invalidates comparison with evidence collected under the previous value.

Warmup queries use the same deterministic query generator as measured queries but consume a distinct deterministic prefix so no measured query is silently reused as a warmup.

## `incremental_peak_memory_mb`

The memory boundary identifier is:

`case-owned-process-tree-above-pre-launch-baseline-v1`

The harness captures its baseline immediately before the case-owned runtime/process is launched. Memory attributable to the harness/control process before that boundary is excluded. All case-owned processes created after the baseline are included using PID plus create-time identity so PID reuse cannot corrupt ownership.

The peak window begins at case start and remains active through setup, warmups and all measured queries. It ends only after the final query memory sample and before teardown.

`incremental_peak_memory_mb` is the peak memory attributable to the owned process tree above the declared pre-launch baseline, reported in decimal megabytes. Linux prefers PSS where available; an RSS fallback must be explicitly represented by the existing memory-accounting/system fingerprint authority. Competing records using different accounting semantics are not comparable.

A missing/empty process scope is invalid evidence, never zero-memory success.

## Identity and scenario binding

Normalized evidence reuses the canonical `EvidenceIdentity` / M0 -> EvidenceContext path. No second SystemState or Scenario fingerprint model is introduced.

A comparable group must have identical:

- host platform and architecture;
- system fingerprint;
- harness schema;
- corpus SHA-256;
- scenario fingerprint;
- setup boundary identifier;
- memory scope identifier;
- warmup query count;
- measured query count;
- core metric units.

The scenario fingerprint authority includes every parameter that changes execution semantics, including normalized warmup count. Virtualized-only parameters remain non-authoritative for native-DOM exactly as required by the evidence-context contract.

## Canonical synthetic corpus authority

The M7 browser corpus is generated in deterministic 1 MiB chunks. Each canonical chunk restarts the declared Unicode byte pattern. Zevryon's case-owned synthetic store must reproduce those exact chunk semantics rather than treating the payload as one unbroken pattern stream.

The admitted Zevryon store builder streams the corpus into a single-record MassiveDoc store after process launch. Store construction is therefore charged to setup, and the emitted store payload SHA-256 must equal the shared deterministic browser corpus SHA-256 before normalized evidence is admissible.

A prebuilt Zevryon store is diagnostic-only and cannot satisfy normalized leadership admission.

## Runtime preflight boundary

Runtime availability checks are not benchmark measurements. A runtime preflight may launch and close exact browser/engine distributions to prove readiness, but the preflight report must explicitly state that measurement did not start.

Preflight is a separate stage rather than an automatic immediate precursor to timed collection. This avoids silently biasing setup measurements through runtime page-cache warming. A later collection admission layer may bind the preflight host/system fingerprint and stable runtime identities to measured evidence. Ephemeral WebDriver ports are transport details and may be normalized for identity matching; engine binary path, version/SHA, distribution and adapter identity may not.

## Current implementation status

As of admitted `main` commit `2b17dd067aa102acd1dab2e20b01b3daa3ff423b`:

- browser cases start normalized setup timing before case-owned runtime launch;
- declared warmups complete before the ready boundary and are part of scenario identity;
- browser successful terminal records carry validated normalized core evidence;
- browser process-tree memory accounting uses PID plus create-time ownership and fails closed on empty scope;
- Zevryon has a persistent benchmark-session path with implementation-local raw query timing;
- Zevryon constructs the exact single-record M7 synthetic store inside the case-owned process after launch;
- the Zevryon synthetic stream is bound to the canonical 1 MiB chunk restart semantics and cross-boundary SHA authority;
- Zevryon normalized memory accounting ends at the final measured query before teardown;
- legacy Zevryon giant-document summaries remain diagnostic only.

Exact-head run `33119457934` validated this admitted tree across the repository's Linux, Windows, Unicode, Apple-removal, Win32 and i386 gates.

The next candidate layer adds a legacy-independent canonical six-browser full-set collector and the separate fixed five-metric evaluator. Those surfaces do not grant leadership by existing in the repository; real comparable evidence is still required.

## Required implementation sequence

1. **ADMITTED** - preserve this boundary contract and the fixed five-metric contract;
2. **ADMITTED** - extend canonical scenario authority with normalized warmup semantics;
3. **ADMITTED** - move browser case start before case-owned runtime launch and publish normalized setup evidence;
4. **ADMITTED** - add one persistent Zevryon benchmark-session path with implementation-local raw query timings;
5. **ADMITTED** - measure both paths with the same process-ownership window semantics;
6. **ADMITTED** - attach validated normalized core evidence to successful terminal records;
7. **IMPLEMENTED / EVIDENCE PENDING** - collect the real canonical six-competitor full set with exact runtime identity and corpus authority;
8. **IMPLEMENTED / EVIDENCE PENDING** - run the separate five-metric leadership evaluator only after complete comparable evidence is admitted.

No implementation step, preflight PASS, unit-test PASS or CI PASS is itself a leadership claim. Leadership eligibility exists only for a complete admitted evidence bundle that satisfies the fixed metric rule.
