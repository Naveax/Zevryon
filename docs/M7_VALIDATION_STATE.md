# M7 exact-head validation state

This file records the validation discipline for the integrated M7 normalized measurement and canonical evidence pipeline.

## Current validation rule

A branch/ref must have at most one active `Windows and Linux CI` run for the current exact head SHA. Before any branch write or rerun, inspect queued and in-progress Actions. If an equivalent run already exists, keep its run ID and do not create another one.

A GitHub Actions `startup_failure`, or a run in which no job executes any step, is infrastructure/orchestration evidence only. It is neither a code PASS nor a code FAIL and cannot be used for admission.

An M7 milestone is admitted only when one exact-head run completes successfully across the workflow's Linux, Windows, Unicode, Apple-removal, Win32 and i386 gates. The admitted `main` commit must preserve the exact validated tree.

CI waiting is not a reason to stop unrelated work. New work may be prepared as unreferenced Git objects/commits while an existing run executes, but no ref movement may create a duplicate active run for the same exact candidate.

## Last admitted milestone

`main` is admitted through:

- commit: `d598b7fbb2db121569325932510378289e5641c1`;
- tree: `eda8388f6fbc7471e2bb785dd7f0019e07060b94`;
- exact-head validation run: `33120298227`;
- run attempt: `1`;
- conclusion: `SUCCESS`;
- validated gates: Linux build/headless, Windows build/headless, Linux Unicode authority, Windows Unicode authority, Apple backend removal guard, real Win32 address-space gate and Linux i386 address-space gate.

That admission includes everything previously admitted through `2b17dd067aa102acd1dab2e20b01b3daa3ff423b`, plus:

- a separate fixed five-metric M7 leadership evaluator;
- a legacy-independent normalized canonical browser full-set collector;
- exact six-browser by two-mode case-matrix authority;
- deterministic top-level, terminal and normalized corpus-SHA binding;
- order-independent full-set validation;
- fail-closed rejection of missing, duplicated, unavailable or non-comparable canonical evidence;
- continued prohibition on leadership claims before the metric gate is separately evaluated.

The legacy Zevryon process-per-query summary remains diagnostic-only.

## Prepared follow-on scope

The next candidate is intentionally prepared as an unadmitted descendant rather than described here by a self-referential commit SHA. Its scope contains:

- exact-runtime launch/readiness preflight for Chrome, Firefox, Edge, WebKit, Servo and Ladybird;
- host/system fingerprint binding between preflight and measurement;
- stable runtime-identity binding that removes only the ephemeral Servo/Ladybird WebDriver port while preserving binary/version/SHA identity;
- collection admission across preflight, canonical 6x2 browser evidence and both Zevryon normalized modes;
- input-artifact SHA-256 receipts;
- a canonical collection runbook;
- single-bundle / no-cherry-pick ranking discipline;
- CTest authority coverage for preflight and collection admission.

This follow-on scope is not admitted until the exact branch head containing it completes one successful validation run. Parent success is not evidence for an unvalidated descendant.

## Canonical evidence discipline

A CI PASS proves repository code/test integrity for the candidate tree. It does not prove M7 leadership.

Leadership evidence additionally requires:

- exact six canonical browser/engine identities: Chrome, Firefox, Edge, WebKit, Servo and Ladybird;
- both `virtualized` and `native-dom` modes for every canonical runtime;
- exact M7 synthetic corpus identity;
- complete normalized setup/query/memory evidence;
- same-system comparability;
- successful exact-runtime preflight when the preflight-bound admission path is used;
- successful collection admission before the five-metric rule is evaluated.

Missing runtime evidence is incomplete evidence, not a zero or a substituted engine. Branded Chrome/Edge may not be replaced by bundled Chromium. Servo/Ladybird may not be replaced by another WebDriver implementation.

One leadership decision consumes one complete evidence bundle. Independent repeat bundles may be collected for diagnostics or reproducibility, but metrics from different bundles may not be mixed, cherry-picked or rerun until a preferred ranking appears. Any future repeated-run aggregation policy must be fixed in the metric contract before the corresponding evidence is collected.

## Historical non-admission runs

- `32984872310`: push-triggered run ended without producing usable job execution evidence.
- `32984846149`: pull-request-triggered duplicate surface ended after its jobs were cancelled; no admission evidence.
- `32985816321`: exact-head push run concluded `startup_failure`; its jobs never produced test execution evidence.

None of these runs should be rerun merely to poll CI state. A new run is justified only by a new exact-head candidate or an explicit infrastructure recovery action after confirming there is no equivalent active run.
