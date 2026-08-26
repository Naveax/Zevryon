# M7 competitor adapter and evidence contract

## Purpose

M7 compares Zevryon with real browser products/engines without collapsing unlike binaries into the same label or silently treating a missing adapter as a passing result.

The canonical competitor set is:

- Google Chrome
- Mozilla Firefox
- Microsoft Edge
- WebKit
- Servo
- Ladybird

Bundled Playwright Chromium may remain as an auxiliary development baseline, but it is not a substitute for branded Chrome or Edge in a leadership claim.

## Identity is part of the evidence

Every successful browser result must record:

- canonical competitor name;
- adapter kind;
- browser/engine version or commit where available;
- binary/channel identity;
- operating system and architecture;
- normalized system-state fingerprint;
- harness version/schema;
- corpus SHA-256;
- scenario fingerprint covering the common scenario parameters.

The system-state, corpus and scenario fingerprints are lowercase SHA-256 values over canonicalized evidence inputs. They are comparison identity, not decorative metadata.

A result may not be relabeled as another browser merely because the rendering engine is related.

In particular:

- Playwright `chromium` is an auxiliary Chromium build, not Google Chrome;
- Playwright can launch installed branded Chrome with channel `chrome` and Edge with channel `msedge`;
- Playwright's Firefox support uses its compatible/patched Firefox build and must be reported with that identity rather than silently asserted to be an arbitrary stock Firefox binary;
- Playwright WebKit is a WebKit build and is not branded Safari.

Reference: https://playwright.dev/python/docs/browsers

## Adapter classes

### Playwright adapter

Initial supported identities:

- auxiliary Chromium: Playwright `chromium`;
- Chrome: Playwright Chromium launcher with channel `chrome`;
- Edge: Playwright Chromium launcher with channel `msedge`;
- Playwright Firefox identity: Playwright `firefox`;
- WebKit: Playwright `webkit`.

Chrome/Edge absence on the host is an explicit availability failure, never a request to fall back to bundled Chromium under the branded name.

### Servo WebDriver adapter

Servo exposes a WebDriver server through `--webdriver=PORT`. The adapter must launch an exact Servo binary/build, establish a WebDriver session, execute the common scenario, capture the exact version/commit, and terminate both session and process cleanly.

Reference: https://github.com/servo/servo/wiki/Control-Servo-using-WebDriver

### Ladybird adapter

Ladybird has an explicit headless/test-mode surface, but M7 must not assume WebDriver parity that has not been demonstrated for the required scenario. The adapter may use a command/headless protocol only for operations it can certify as equivalent. Missing controls are reported as unsupported capability rather than emulated with a different workload.

Reference: https://github.com/LadybirdBrowser/ladybird/blob/master/Documentation/Testing.md

## Result states

Every requested engine/mode produces exactly one terminal state:

- `success`: the common scenario completed and required evidence is present;
- `unsupported`: the exact requested capability is not implemented by the adapter/browser;
- `unavailable`: the adapter exists but the required binary/channel is absent on the host;
- `timeout`: the bounded case exceeded its declared timeout;
- `error`: execution failed for another recorded reason;
- `invalid`: evidence is incomplete or violates the comparison contract.

Unsupported or unavailable cases are data, not success. They remain visible in the report and make full-set coverage incomplete.

A raw result cannot bypass the evidence validator by claiming `success` directly. Successful terminal evidence is invalid unless all required identity/comparison fields are present and well-formed.

## Common scenario invariants

A comparable case uses the same:

- host platform and architecture;
- normalized system-state fingerprint;
- payload bytes and corpus hash;
- Unicode payload generator;
- viewport dimensions;
- virtualized slice size;
- deterministic query/scroll sequence;
- warmup policy;
- setup/query timing boundaries;
- timeout policy;
- process-tree memory accounting definition;
- harness schema and scenario fingerprint.

Adapter-specific setup is recorded separately from workload timing.

A browser-native DOM layout and Zevryon's current deterministic average-advance layout are not the same rendering workload. Results may be published side-by-side, but a leadership metric may combine them only after the metric contract demonstrates equivalence.

## Memory evidence

Process memory must be scoped to the browser/engine process tree created for the case. Harness baseline, post-setup resident memory, post-query resident memory, peak memory, and incremental peak above harness baseline are recorded separately.

If an adapter cannot identify the relevant process tree reliably, its memory metric is `invalid` rather than estimated from an unrelated parent process.

## Coverage and leadership gates

The report must distinguish:

- requested competitors;
- available competitors;
- successfully measured competitors;
- auxiliary/non-canonical baselines.

No leadership claim is admitted when a required core competitor is silently missing, mislabeled, measured on mismatched comparison identity, or measured under a materially different workload.

Leadership is explicitly two-stage and fail-closed:

1. the evidence gate requires every canonical competitor to complete successfully with matching host platform/architecture, system fingerprint, harness schema, corpus SHA-256 and scenario fingerprint;
2. only a separate metric evaluator may then evaluate the performance leadership rule.

Passing the evidence gate is **not** a leadership result. The registry/coverage layer must report the metric gate as unevaluated and final leadership eligibility as false until the metric evaluator has actually processed the raw measurements.

The existing M7 performance rule remains authoritative: Zevryon must be first in at least four core efficiency metrics and within 5% of the leader in every remaining core metric before a leadership claim is allowed.

## Migration of the existing harness

`scripts/browser_competitor_benchmark.py` currently hard-codes Playwright Chromium and Firefox. The implementation sequence is:

1. introduce an explicit competitor registry and requested-engine list;
2. preserve bundled Chromium as an auxiliary baseline;
3. add real WebKit plus branded Chrome/Edge channels with exact identity receipts;
4. split `unsupported`, `unavailable`, `timeout`, `error`, and `invalid` states;
5. add Servo WebDriver adapter;
6. add only the Ladybird controls that can execute the same certified scenario;
7. make full-set evidence coverage machine-readable while keeping the final metric gate separate;
8. publish raw per-case evidence before any summary ranking.
