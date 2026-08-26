# M7 core efficiency metric contract

## Purpose

`docs/MAINLINE_EXECUTION_PLAN.md` requires Zevryon to be first in at least four core efficiency metrics and within 5% of the leader in every remaining core metric before a leadership claim is allowed. The plan does not enumerate those metrics, so the metric set must be fixed before any evaluator can issue a leadership decision.

This contract defines the core set without pretending that the current Zevryon and browser timing boundaries are already equivalent.

## Core metric set

The canonical M7 leadership set contains exactly five lower-is-better metrics:

1. `setup_to_ready_seconds`
   - wall-clock time from the declared cold case start boundary until the implementation is ready to execute the first measured query;
   - adapter/process launch, source/store open, payload preparation, index/checkpoint preparation and any required warmup must be included or excluded identically according to the scenario contract;
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

A metric value is leadership-admissible only when the corresponding case already passes the M7 evidence/comparability gate and the metric itself has a common measurement boundary.

In particular, the current giant-document harness is not yet sufficient for the final metric gate because it explicitly reports different timing semantics:

- Zevryon checkpoint query timings currently include CLI process start, store open, checkpoint open, bounded read and JSON serialization;
- browser query timings currently measure steady-context page work after browser and payload setup;
- browser memory is reported as incremental process-tree PSS above the Python/Playwright harness baseline, while the Zevryon summary does not yet publish the same normalized process-tree baseline contract.

Therefore the evaluator must reject legacy/non-normalized fields rather than compare them numerically as though they were equivalent.

## Required normalized evidence

Every competitor result admitted to the final metric gate must publish all five core fields under the same scenario fingerprint plus:

- raw per-query samples used to compute P50/P95/P99;
- sample count;
- declared warmup count and whether warmups are excluded from percentiles;
- exact setup start and ready boundaries;
- exact process-tree ownership rule;
- memory baseline boundary;
- units and schema version.

No percentile may be copied from a differently filtered sample set.

## Ranking rule

For each core metric:

- discard no successful canonical competitor silently;
- if any required canonical competitor lacks a valid comparable value, the metric gate is incomplete;
- the leader value is the minimum valid value;
- Zevryon is `first` when its value equals the minimum value; exact ties are co-leadership and count as first;
- otherwise Zevryon is `within_5_percent` only when `zevryon_value <= leader_value * 1.05`;
- negative, non-finite or dimensionally invalid values are invalid evidence;
- a zero leader value is admissible only when zero is physically meaningful under the metric contract; otherwise the evidence is invalid rather than divided by zero.

Final leadership eligibility requires:

- all five metrics complete and comparable;
- Zevryon first in at least four of the five;
- Zevryon no worse than 5% above the leader in every metric where it is not first.

## Deliberate boundary

This document defines the metric authority only. It does not claim that current legacy benchmark output already satisfies the normalized field contract, and it does not grant leadership eligibility by itself.

The implementation order remains:

1. competitor/evidence authority;
2. identity-preserving adapters;
3. explicit benchmark planning and real adapter routing;
4. normalized common timing/memory evidence;
5. only then the machine-readable five-metric leadership evaluator.
