# M7 competitor laboratory evidence contract

## Purpose

M7 compares Zevryon with Chrome, Firefox, Edge, WebKit, Servo and Ladybird using the same corpus and operation definitions. This first slice defines the evidence format and a fail-closed leadership gate before engine-specific launch adapters are added.

## Canonical engines

Every campaign must declare a status for:

- Zevryon
- Chrome
- Firefox
- Edge
- WebKit
- Servo
- Ladybird

Statuses are `measured`, `unsupported`, `failed` or `missing`. Unsupported entries require a reason. Failed and missing competitors block a leadership claim. At least four non-Zevryon competitors must be measured even when some canonical engines are legitimately unsupported on the campaign platform.

## Raw run identity

Each raw run records:

- engine and exact engine version;
- corpus SHA-256 and logical byte count;
- UTC capture time and run index;
- OS, release, architecture, CPU model and physical RAM;
- thermal state and power mode;
- all canonical metrics for a successful run;
- a failure mode when the run did not complete.

A failed run may have no metrics because a crash or timeout can occur before measurement. It is still retained in the campaign evidence and blocks leadership admission.

Measured engines are aggregated only when they use the same corpus identity and comparable system state. A single engine/version needs at least five successful runs. Duplicate run indices are rejected.

## Canonical core metrics

The M7 leadership comparison uses the existing Zevryon performance-contract surfaces:

| Metric | Direction |
| --- | --- |
| process-group PSS | lower |
| first viewport, preindexed | lower |
| first viewport, streaming | lower |
| scroll P99 | lower |
| maximum normal stall | lower |
| exact search, warm | lower |
| exact search, cold | lower |
| mutation P95 | lower |
| copy throughput | higher |

For every metric the engine aggregate publishes median, nearest-rank P95 and nearest-rank P99. Leadership comparison uses the median while retaining P95/P99 as raw tail evidence.

## Leadership gate

`leadership_claim_allowed` is true only when all of the following hold:

1. every canonical engine has an explicit status;
2. all measured engines use the same corpus and comparable system state;
3. no measured benchmark run failed;
4. at least four competitors are measured;
5. no canonical competitor is `failed` or `missing`;
6. Zevryon is first or tied for first in at least four core metrics;
7. on every remaining core metric Zevryon is within 5% of the measured leader.

This implements the mainline rule that a leadership claim is forbidden until Zevryon wins at least four core efficiency metrics and stays within 5% everywhere else. The extra minimum-competitor and failed-run checks are deliberately stricter so absence or instability cannot be turned into a marketing victory by subtraction.

## Tools

`zevryon_platform/competitor_lab.py` owns validation, aggregation, campaign hashing and leadership evaluation.

`scripts/evaluate_m7_competitor_lab.py` reads a campaign JSON and writes a deterministic evidence report. `--require-leadership` returns a nonzero exit status when the evidence is valid but the leadership gate is not met.

`m7-competitor-lab-contract-smoke` is registered with CTest when a Python interpreter is available. It uses only the Python standard library.

## Not yet certified

This slice does not claim that Chrome, Firefox, Edge, WebKit, Servo or Ladybird have been benchmarked. Engine launch/control adapters, corpus delivery, browser memory accounting and physical raw runs remain later M7 execution slices.
