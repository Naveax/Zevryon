# M7 exact-head validation state

This file records the validation discipline for the integrated M7 normalized measurement and canonical evidence pipeline.

## Current validation rule

A branch/ref must have at most one active `Windows and Linux CI` run for the current exact head SHA. Before any branch write or rerun, inspect queued and in-progress Actions. If an equivalent run already exists, keep its run ID and do not create another one.

A GitHub Actions `startup_failure`, or a run in which no job executes any step, is infrastructure/orchestration evidence only. It is neither a code PASS nor a code FAIL and cannot be used for admission.

An M7 milestone is admitted only when one exact-head run completes successfully across the workflow's Linux, Windows, Unicode, Apple-removal, Win32 and i386 gates. The admitted `main` commit must preserve the exact validated tree.

CI waiting is not a reason to stop unrelated work. New work may be prepared as unreferenced Git objects/commits while an existing run executes, but no ref movement may create a duplicate active run for the same exact candidate.

## Last admitted milestone

`main` is admitted through:

- commit: `2b17dd067aa102acd1dab2e20b01b3daa3ff423b`;
- tree: `e18a10ecb74edf98d4eeef5e81764c551467402a`;
- exact-head validation run: `33119457934`;
- run attempt: `1`;
- conclusion: `SUCCESS`;
- validated gates: Linux build/headless, Windows build/headless, Linux Unicode authority, Windows Unicode authority, Apple backend removal guard, real Win32 address-space gate and Linux i386 address-space gate.

That admission includes:

- normalized Playwright/WebDriver browser case execution;
- setup timing beginning before case-owned runtime launch;
- warmup count bound into scenario identity;
- parent-runner normalized evidence revalidation;
- persistent Zevryon benchmark-session execution;
- case-owned single-record M7 synthetic store construction after process launch;
- exact 1 MiB canonical synthetic chunk restart semantics;
- cross-boundary corpus SHA authority;
- process-tree peak memory accounting through the final measured query and before teardown.

The legacy Zevryon process-per-query summary remains diagnostic-only.

## Current next exact-head candidate

The next referenced candidate is:

- branch: `agent/m7-normalized-benchmark-runner-current-main-v1`;
- commit: `d598b7fbb2db121569325932510378289e5641c1`;
- tree: `eda8388f6fbc7471e2bb785dd7f0019e07060b94`;
- exact-head run ID: `33120298227`;
- scope: separate fixed five-metric evaluator plus legacy-independent normalized canonical six-browser x two-mode full-set collector.

Do not admit this candidate based on partial job success. Its exact run must reach a final successful conclusion. Do not rerun or redispatch it merely to poll state.

Additional follow-on work may remain unreferenced until this exact-head candidate settles. Unreferenced commits are not admission evidence and must not be described as validated merely because their parent passed CI.

## Canonical evidence discipline

A CI PASS proves repository code/test integrity for the candidate tree. It does not prove M7 leadership.

Leadership evidence additionally requires:

- exact six canonical browser/engine identities: Chrome, Firefox, Edge, WebKit, Servo and Ladybird;
- both `virtualized` and `native-dom` modes for every canonical runtime;
- exact M7 synthetic corpus identity;
- complete normalized setup/query/memory evidence;
- same-system comparability;
- separately bound runtime-preflight identity when a preflight is used;
- successful collection admission before the five-metric rule is evaluated.

Missing runtime evidence is incomplete evidence, not a zero or a substituted engine. Branded Chrome/Edge may not be replaced by bundled Chromium. Servo/Ladybird may not be replaced by another WebDriver implementation.

## Historical non-admission runs

- `32984872310`: push-triggered run ended without producing usable job execution evidence.
- `32984846149`: pull-request-triggered duplicate surface ended after its jobs were cancelled; no admission evidence.
- `32985816321`: exact-head push run concluded `startup_failure`; its jobs never produced test execution evidence.

None of these runs should be rerun merely to poll CI state. A new run is justified only by a new exact-head candidate or an explicit infrastructure recovery action after confirming there is no equivalent active run.
