# M7 exact-head validation state

This file records the validation discipline for the integrated M7 normalized measurement and canonical evidence pipeline.

## Current validation rule

A branch/ref must have at most one active `Windows and Linux CI` run for the current exact head SHA. Before any branch write or rerun, inspect queued and in-progress Actions. If an equivalent run already exists, keep its run ID and do not create another one.

A GitHub Actions `startup_failure`, or a run in which no job executes any step, is infrastructure/orchestration evidence only. It is neither a code PASS nor a code FAIL and cannot be used for admission.

An M7 milestone is admitted only when one exact-head run completes successfully across the workflow's Linux, Windows, Unicode, Apple-removal, Win32 and i386 gates. The admitted `main` commit must preserve the exact validated tree.

CI waiting is not a reason to stop unrelated work. New work may be prepared as unreferenced Git objects/commits while an existing run executes, but no ref movement may create a duplicate active run for the same exact candidate.

## Last admitted milestone

`main` is admitted through:

- commit: `fcd211776675993a8ce7ad0954f2134b24389143`;
- tree: `4352204712eb5956a2417d8c3494f93a0d370a87`;
- exact-head validation run: `33125276373`;
- run attempt: `1`;
- conclusion: `SUCCESS`;
- validated gates: Linux build/headless, Windows build/headless, Linux Unicode authority, Windows Unicode authority, Apple backend removal guard, real Win32 address-space gate and Linux i386 address-space gate.

That admission includes everything previously admitted through `6ea7a74123069dbdb035bd59cf93a3f870f85a9d`, plus:

- M0-backed system-fingerprint v2 using OS/release, architecture, logical CPU count, CPU model, physical RAM and device class;
- stronger Windows CPU model discovery through the existing M0 benchmark metadata authority;
- preservation of the full M0 machine/thermal receipt alongside the stable fingerprint;
- explicit physical-host certification for M7 leadership evidence using the existing M0 `physical_certification_checks()` rule;
- mandatory physical-device confirmation and observed thermal evidence at runtime-preflight and browser-full-set collection boundaries;
- canonical `m7_zevryon_physical_case.py` capture of certified raw M0 machine/thermal receipts immediately before and after both normalized Zevryon modes;
- collection admission that recomputes all six physical-host receipts and rejects forged embedded Zevryon physical evidence;
- canonical publication-manifest authority binding raw evidence hashes, physical receipts, runtime identities, evaluator result and exact clean Git commit/tree.

The push of this exact admitted SHA to `main` may produce its own workflow run. That run is additional main-ref CI evidence; it is not a reason to rerun the already successful branch exact-head validation.

## Prepared follow-on scope

The next child is intentionally unadmitted until its own exact-head CI succeeds. It adds publication replay and final pre-evidence schema freezing:

- physical-host collection admission schema `zevryon.competitor.collection-admission.v2`;
- replay-capable publication schema `zevryon.competitor.evidence-bundle-manifest.v2`;
- re-hash all four canonical raw artifacts;
- constrain the admission artifact and every raw artifact path to the declared `artifact_root` after canonical path resolution;
- reject relative traversal, absolute out-of-root paths and other resolved path escapes before evidence is read;
- re-read runtime preflight, browser 6x2 evidence and both physical Zevryon mode artifacts;
- recompute `admit_collection()` from those four raw artifacts;
- require the stored admission JSON to equal the recomputed admission field-for-field, apart from its explicit `input_artifacts` receipts;
- require the replay receipt to carry the exact canonical recomputed-field set and four raw artifact SHA-256 values;
- bind that replay receipt into the immutable publication-manifest payload;
- require replay SHA receipts to equal the manifest's independently verified artifact SHA receipts;
- preserve both Zevryon before/after physical-host receipts in manifest validation.

Parent success is not evidence for this follow-on child. It requires one new exact-head validation run after its ref is advanced.

## Canonical evidence discipline

A CI PASS proves repository code/test integrity for the candidate tree. It does not prove M7 leadership.

Leadership evidence additionally requires:

- exact six canonical browser/engine identities: Chrome, Firefox, Edge, WebKit, Servo and Ladybird;
- both `virtualized` and `native-dom` modes for every canonical runtime;
- exact M7 synthetic corpus identity;
- complete normalized setup/query/memory evidence;
- same-system comparability under the canonical system fingerprint;
- successful exact-runtime preflight;
- successful M0 physical-host certification for preflight, browser collection and both before/after Zevryon case boundaries;
- successful collection admission before the five-metric result is published;
- publication replay from the exact four raw artifacts before a canonical manifest can be produced.

Missing runtime evidence is incomplete evidence, not a zero or a substituted engine. Branded Chrome/Edge may not be replaced by bundled Chromium. Servo/Ladybird may not be replaced by another WebDriver implementation.

One leadership decision consumes one complete evidence bundle. Independent repeat bundles may be collected for diagnostics or reproducibility, but metrics from different bundles may not be mixed, cherry-picked or rerun until a preferred ranking appears. Any future repeated-run aggregation policy must be fixed in the metric contract before the corresponding evidence is collected.

## Historical non-admission runs

- `32984872310`: push-triggered run ended without producing usable job execution evidence.
- `32984846149`: pull-request-triggered duplicate surface ended after its jobs were cancelled; no admission evidence.
- `32985816321`: exact-head push run concluded `startup_failure`; its jobs never produced test execution evidence.

None of these runs should be rerun merely to poll CI state. A new run is justified only by a new exact-head candidate or an explicit infrastructure recovery action after confirming there is no equivalent active run.
