# M7 core efficiency metric contract

## Purpose

`docs/MAINLINE_EXECUTION_PLAN.md` requires Zevryon to be first in at least four core efficiency metrics and within 5% of the leader in every remaining core metric before a leadership claim is allowed. The plan does not enumerate those metrics, so the metric set must be fixed before any evaluator can issue a leadership decision.

This contract defines the core set and the ranking discipline. Legacy/non-normalized fields remain diagnostic even when they use similar units.

## Core metric set

The canonical M7 leadership set contains exactly five lower-is-better metrics:

1. `setup_to_ready_seconds`
   - wall-clock time from the declared cold **case** start boundary until the implementation is ready to execute the first measured query;
   - adapter/process launch, source/store open or construction, payload preparation, index/checkpoint preparation and required warmup are included according to the normalized lifecycle contract;
   - implementation-specific setup subphases may be reported separately but do not replace this normalized field.

2. `query_milliseconds_p50`
   - median latency across the declared deterministic measured query sequence.

3. `query_milliseconds_p95`
   - P95 latency across that same measured sequence.

4. `query_milliseconds_p99`
   - P99 latency across that same measured sequence.

5. `incremental_peak_memory_mb`
   - peak resident/process-tree memory attributable to the case above the declared harness/process baseline;
   - the process-tree scope and baseline sampling boundary must be identical across competitors for a leadership comparison.

All five metrics are lower-is-better.

### Meaning of cold case start

`cold case start` means the case-owned runtime/process, implementation-specific source/store, index/checkpoint state and measured-query session do not exist before the declared case start boundary.

It does **not** silently claim that the operating-system page cache, filesystem cache, CPU caches or thermal state have been globally purged. Those machine-state conditions must be symmetric under the declared benchmark system-state discipline. An implementation may not receive a private pre-launched runtime or prebuilt implementation-specific source state while another pays that work inside setup.

## Diagnostic but non-core fields

The following may be published and are useful for debugging, but they do not count toward the four-of-five leadership rule unless this contract is explicitly revised before evidence collection:

- arithmetic mean query latency;
- single worst/max query latency;
- post-setup resident memory;
- post-query resident memory;
- raw absolute process-tree peak memory;
- implementation-specific checkpoint/index build subphase time;
- source bytes read and source-read reduction;
- checkpoint physical bytes and checkpoint overhead ratio;
- implementation-specific speedup ratios.

This prevents a result from manufacturing extra "core metrics" after measurement in order to satisfy the leadership threshold.

## Comparison authority

A metric value is leadership-admissible only when the corresponding case already passes the M7 evidence/comparability gate and the metric itself has the common normalized measurement boundary from `docs/M7_NORMALIZED_MEASUREMENT_CONTRACT.md`.

The admitted normalized path uses:

- browser setup timing beginning immediately before case-owned runtime launch;
- browser implementation-local page/engine query timings after the post-warmup ready boundary;
- Zevryon implementation-local query timings inside one persistent case-owned benchmark process;
- case-owned process-tree peak memory above the pre-launch harness baseline for both paths;
- exact deterministic M7 synthetic corpus identity;
- the same normalized evidence schema and identity authority.

Legacy Zevryon process-per-query/checkpoint-window timing and legacy giant-document summaries do not satisfy this comparison authority and cannot feed the final metric gate.

## Required normalized evidence

Every competitor result admitted to the final metric gate must publish all five core fields under the same comparable scenario authority plus:

- raw per-query samples used to compute P50/P95/P99;
- sample count;
- declared warmup count and proof that warmups are excluded from percentiles;
- exact setup start and ready boundaries;
- exact process-tree ownership rule;
- memory baseline boundary;
- units and schema version;
- exact corpus SHA-256;
- exact runtime identity evidence required by the canonical adapter contract.

No percentile may be copied from a differently filtered sample set.

## Repeat-run and cherry-pick discipline

The current fixed leadership gate evaluates **one complete admitted evidence bundle** for one declared host/system/runtime/scenario state. A complete bundle contains all six canonical competitors in both required modes plus both normalized Zevryon modes.

Additional complete runs may be collected for reproducibility analysis, but they remain separate bundles unless a repetition/aggregation contract is fixed **before** those measurements are collected.

The following are not permitted:

- rerunning an unchanged valid bundle merely because Zevryon did not meet the leadership threshold;
- selecting the best setup result from one run and the best query/memory values from another;
- selecting the most favorable pass per competitor;
- silently discarding a valid but slower complete pass;
- introducing median-of-runs, best-of-N, trimmed means or other cross-run aggregation after seeing results.

If repeated-run aggregation is desired for a future leadership gate, the repetition count, execution order, system-state treatment, aggregation function and failure handling must be declared in a contract revision before the first evidence in that series is collected. Existing single-bundle evidence cannot be retroactively reclassified into such a series.

An invalid or incomplete run may be replaced only after its invalidity is preserved and explained; replacement does not turn the invalid run into leadership evidence.

## Ranking rule

For each core metric:

- discard no successful canonical competitor silently;
- if any required canonical competitor lacks a valid comparable value, the metric gate is incomplete;
- the leader value is the minimum valid value;
- Zevryon is `first` when its value equals the minimum value; exact ties are co-leadership and count as first;
- otherwise Zevryon is `within_5_percent` only when `zevryon_value <= leader_value * 1.05`;
- negative, non-finite or dimensionally invalid values are invalid evidence;
- a zero leader value is admissible only when zero is physically meaningful under the metric contract; for the current five measured efficiency metrics a zero observed leader is treated as invalid evidence by the evaluator.

Final leadership eligibility requires:

- all five metrics complete and comparable;
- Zevryon first in at least four of the five;
- Zevryon no worse than 5% above the leader in every metric where it is not first;
- the mode-specific rule to pass independently for both `virtualized` and `native-dom` in the current evaluator.

## Deliberate boundary

This document defines metric and ranking authority. It does not grant leadership eligibility because a benchmark implementation, unit test, runtime preflight or CI run exists.

The implementation/evidence order is:

1. competitor/evidence authority;
2. identity-preserving adapters;
3. explicit benchmark planning and real adapter routing;
4. normalized common timing/memory evidence;
5. exact canonical evidence collection and admission;
6. only then the machine-readable five-metric leadership evaluator.
