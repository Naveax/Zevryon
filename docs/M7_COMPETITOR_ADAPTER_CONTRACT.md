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

Current Servo exposes a WebDriver server from `servoshell` through the headless launch surface `--headless --webdriver=PORT about:blank`. The adapter resolves an exact Servo/servoshell binary, records `--version` output as runtime identity, launches the bounded WebDriver service, and uses the shared W3C transport.

The existence of the service does not itself make a benchmark case executable. The planner remains fail-closed until the exact giant-document common scenario, viewport authority, process-scope memory authority, timeout behavior, and cleanup path are wired and tested together.

Reference source: https://github.com/servo/servo/tree/main/components/webdriver_server

### Ladybird WebDriver adapter

Current Ladybird includes a dedicated `WebDriver` service. Its supported launch surface includes `--headless`, `-l/--listen-address`, and `-p/--port`; Zevryon binds the service to `127.0.0.1` for benchmark control. Ladybird's own performance tooling creates W3C sessions and uses navigation plus `execute/sync`, so M7 no longer classifies Ladybird as a generic command-only headless adapter.

The Ladybird WebDriver executable does not currently expose a reliable `--version` identity surface. Zevryon therefore binds Ladybird runtime identity to the exact resolved WebDriver binary path plus the lowercase SHA-256 of that binary. Missing or unreadable binary identity is `unavailable`/`invalid`, never an inferred version.

As with Servo, the presence of W3C WebDriver is not sufficient for admission. Ladybird remains non-executable in the benchmark planner until the same certified giant-document scenario is wired end-to-end.

Reference sources:

- https://github.com/LadybirdBrowser/ladybird/tree/master/Services/WebDriver
- https://github.com/LadybirdBrowser/ladybird/blob/master/Meta/measure-style-load.py

### Shared W3C WebDriver transport

Servo and Ladybird use one stdlib-only W3C transport authority instead of separate ad-hoc HTTP clients. The shared layer is responsible for:

- endpoint reachability and `/status` receipts;
- `POST /session` creation with an explicit capabilities object;
- session ID and capability validation;
- script/page-load timeout configuration;
- `GET/POST /session/{id}/window/rect`;
- navigation through `/session/{id}/url`;
- synchronous script execution through `/session/{id}/execute/sync`;
- idempotent `DELETE /session/{id}` cleanup;
- distinction between HTTP/transport failures and valid W3C protocol error responses.

A W3C error encoded as `value.error` remains a protocol error even when transported with HTTP 4xx. It must not be collapsed into an opaque network failure.

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

Setting an outer W3C window rectangle is not sufficient viewport evidence. After rect configuration the harness must measure the real page `innerWidth` and `innerHeight` through the same session and reject the case as `invalid` if the certified viewport cannot be established.

A browser-native DOM layout and Zevryon's current deterministic average-advance layout are not the same rendering workload. Results may be published side-by-side, but a leadership metric may combine them only after the metric contract demonstrates equivalence.

## Memory evidence

Process memory must be scoped to the browser/engine process tree created for the case. Harness baseline, post-setup resident memory, post-query resident memory, peak memory, and incremental peak above harness baseline are recorded separately.

Process identity is PID-reuse-safe: an admitted process identity includes both PID and creation time. A recycled PID with a mismatched creation time is not the same browser process.

The live process-tree sampler may use an optional platform/process dependency, but absence of that dependency must fail closed. Pure authority/contract tests must not require the live sampler dependency merely to import or validate deterministic process-scope logic.

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

`scripts/browser_competitor_benchmark.py` began with Playwright Chromium and Firefox. The canonical implementation sequence is now:

1. explicit competitor registry and requested-engine list;
2. bundled Chromium preserved as an auxiliary baseline;
3. real WebKit plus branded Chrome/Edge channels with exact identity receipts;
4. explicit `unsupported`, `unavailable`, `timeout`, `error`, and `invalid` terminal states;
5. deterministic corpus/scenario/system evidence and raw per-case receipts;
6. PID-reuse-safe browser process-scope memory authority;
7. Servo and Ladybird exact-binary launch/identity adapters;
8. one shared W3C WebDriver transport for Servo and Ladybird;
9. common-scenario runtime wiring with verified inner viewport and process-scope accounting;
10. full-set evidence coverage followed by the separate metric evaluator and only then any ranking claim.
