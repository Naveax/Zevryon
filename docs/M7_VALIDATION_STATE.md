# M7 exact-head validation state

This file records the validation discipline for the integrated M7 normalized measurement and canonical evidence pipeline.

## Current validation rule

A branch/ref must have at most one active `Windows and Linux CI` run for the current exact head SHA. Before any branch write or rerun, inspect queued and in-progress Actions. If an equivalent run already exists, keep its run ID and do not create another one.

A GitHub Actions `startup_failure`, or a run in which no job executes any step, is infrastructure/orchestration evidence only. It is neither a code PASS nor a code FAIL and cannot be used for admission.

An M7 milestone is admitted only when one exact-head run completes successfully across the workflow's Linux, Windows, Unicode, Apple-removal, Win32 and i386 gates. The admitted `main` commit must preserve the exact validated tree.

CI waiting is not a reason to stop unrelated work. New work may be prepared as unreferenced Git objects/commits while an existing run executes, but no ref movement may create a duplicate active run for the same exact candidate.

## Last admitted milestone

`main` is admitted through:

- commit: `c8b083d3cee84c634bc772f9e350e6566601efba`;
- tree: `3aa0080faaa0eb22536fd0857627027abe169ed0`;
- exact-head validation run: `33126282659`;
- run attempt: `1`;
- conclusion: `SUCCESS`;
- validated gates: Linux build/headless, Windows build/headless, Linux Unicode authority, Windows Unicode authority, Apple backend removal guard, real Win32 address-space gate and Linux i386 address-space gate.

That admission includes everything previously admitted through `fcd211776675993a8ce7ad0954f2134b24389143`, plus:

- physical-host collection admission schema `zevryon.competitor.collection-admission.v2`;
- replay-capable publication schema `zevryon.competitor.evidence-bundle-manifest.v2`;
- raw-artifact admission replay from runtime preflight, browser 6x2 evidence and both physical Zevryon modes;
- canonical `artifact_root` containment for the admission artifact and all four raw artifacts;
- rejection of relative traversal, absolute out-of-root paths and resolved path escapes before publication reads evidence;
- exact stored-admission versus recomputed-admission equality apart from explicit raw-artifact receipts;
- replay receipts carrying the canonical recomputed-field set and raw artifact SHA-256 values;
- immutable publication binding of replay evidence, physical-host receipts, runtime identities, evaluator result and exact clean Git commit/tree.

The push of this exact admitted SHA to `main` may produce its own workflow run. That run is additional main-ref CI evidence; it is not a reason to rerun the already successful branch exact-head validation.

## Prepared follow-on scope

The next child remains unadmitted until its own exact-head CI succeeds. It closes the remaining physical-browser-stage and artifact-snapshot asymmetries before real evidence collection:

- canonical `m7_physical_browser_full_set.py` wrapper around the normalized six-browser x two-mode collector;
- certified M0 machine/thermal receipts immediately before and after the complete browser full-set stage;
- stable-fingerprint binding of both browser stage receipts to the normalized report host;
- explicit rejection of missing pre/post physical certification, missing thermal observation, post-stage machine drift and forged embedded receipts;
- one-read JSON artifact snapshots so SHA-256, byte count and parsed JSON all derive from the same bytes;
- collection admission schema `zevryon.competitor.collection-admission.v3` requiring the physical browser wrapper and preserving browser before/after receipts;
- atomic replay schema `zevryon.competitor.collection-admission-replay.v2` requiring raw SHA-256 plus byte-count equality;
- a canonical SHA-256 over the exact recomputed admission authority fields;
- publication schema `zevryon.competitor.evidence-bundle-manifest.v3` binding the admission artifact snapshot, raw SHA/byte-count receipts, replay admission-core hash, browser and Zevryon physical-stage evidence, runtime identities, evaluator result and exact clean Git commit/tree;
- standalone manifest reconstruction of the v3 admission core before accepting `manifest_payload_sha256`.

The browser physical wrapper uses before/after receipts for the whole 6x2 stage rather than pretending a static Windows thermal environment override is a live per-case sensor. Any future per-case thermal contract requires a real live provider or another precommitted observation authority.

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
- successful M0 physical-host certification for preflight, browser stage before/after, and both before/after Zevryon case boundaries;
- successful collection admission before the five-metric result is published;
- publication replay from the exact four raw artifacts before a canonical manifest can be produced.

Missing runtime evidence is incomplete evidence, not a zero or a substituted engine. Branded Chrome/Edge may not be replaced by bundled Chromium. Servo/Ladybird may not be replaced by another WebDriver implementation.

One leadership decision consumes one complete evidence bundle. Independent repeat bundles may be collected for diagnostics or reproducibility, but metrics from different bundles may not be mixed, cherry-picked or rerun until a preferred ranking appears. Any future repeated-run aggregation policy must be fixed in the metric contract before the corresponding evidence is collected.

## Historical non-admission runs

- `32984872310`: push-triggered run ended without producing usable job execution evidence.
- `32984846149`: pull-request-triggered duplicate surface ended after its jobs were cancelled; no admission evidence.
- `32985816321`: exact-head push run concluded `startup_failure`; its jobs never produced test execution evidence.

None of these runs should be rerun merely to poll CI state. A new run is justified only by a new exact-head candidate or an explicit infrastructure recovery action after confirming there is no equivalent active run.
