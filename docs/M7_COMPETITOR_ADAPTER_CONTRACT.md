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
- exact binary/channel identity;
- operating system and architecture;
- normalized system-state fingerprint;
- harness version/schema;
- corpus SHA-256;
- scenario fingerprint covering the common scenario parameters.

The system-state, corpus and scenario fingerprints are lowercase SHA-256 values over canonicalized evidence inputs. They are comparison identity, not decorative metadata.

A result may not be relabeled as another browser merely because the rendering engine is related.

In particular:

- Playwright `chromium` is an auxiliary Chromium build, not Google Chrome;
- installed Google Chrome is launched through channel `chrome`;
- installed Microsoft Edge is launched through channel `msedge`;
- Playwright Firefox is reported with its actual Playwright-compatible runtime identity rather than silently asserted to be an arbitrary stock Firefox binary;
- Playwright WebKit is WebKit evidence and is not branded Safari evidence.

Reference: https://playwright.dev/python/docs/browsers

## Adapter classes

### Playwright adapter

Supported identities are auxiliary Chromium, branded Chrome, branded Edge, Playwright Firefox, and Playwright WebKit.

Chrome/Edge absence on the host is an explicit `unavailable` result. The harness never falls back to bundled Chromium under a branded browser name.

Playwright control starts before the browser-process baseline is captured. The already-running Playwright driver therefore belongs to the control baseline and is excluded from browser memory. Only descendants created after that baseline are admitted to browser process-scope evidence.

### Servo WebDriver adapter

Servo exposes a WebDriver server through the headless launch surface `--headless --webdriver=PORT about:blank`.

The adapter:

- resolves an exact Servo/servoshell binary;
- records `--version` output as runtime identity;
- binds control to a loopback WebDriver endpoint;
- creates a W3C session;
- runs the shared giant-document scenario;
- records exact inner-viewport and process-scope evidence;
- closes the session and engine process deterministically.

The planner routes Servo through the WebDriver case executor. A missing binary is `unavailable`; malformed identity or invalid evidence is `invalid`; an unsupported W3C operation is `unsupported`. None of those states may be rewritten as success.

Reference source: https://github.com/servo/servo/tree/main/components/webdriver_server

### Ladybird WebDriver adapter

Ladybird includes a dedicated `WebDriver` service with a headless loopback launch surface. Ladybird's own performance tooling creates W3C sessions and uses navigation/script execution, so M7 treats it as an explicit WebDriver adapter rather than a generic command-only headless browser.

The Ladybird WebDriver executable does not expose a sufficiently reliable version surface for canonical identity. Zevryon therefore binds runtime identity to the exact resolved WebDriver binary path plus the lowercase SHA-256 of that binary.

The planner routes Ladybird through the same WebDriver case executor as Servo. Missing or unreadable binary identity is `unavailable`/`invalid`; unsupported session operations remain `unsupported`.

Reference sources:

- https://github.com/LadybirdBrowser/ladybird/tree/master/Services/WebDriver
- https://github.com/LadybirdBrowser/ladybird/blob/master/Meta/measure-style-load.py

### Shared W3C WebDriver transport

Servo and Ladybird use one stdlib-only W3C transport authority. It owns:

- `/status` readiness receipts;
- `POST /session` creation with explicit capabilities;
- session ID and capability validation;
- script/page-load timeout configuration;
- window rectangle configuration;
- navigation through `/session/{id}/url`;
- synchronous execution through `/session/{id}/execute/sync`;
- callback-based asynchronous execution through `/session/{id}/execute/async`;
- idempotent `DELETE /session/{id}` cleanup;
- distinction between HTTP/transport failures and valid W3C protocol errors.

A W3C error encoded as `value.error` remains a protocol error even when transported with HTTP 4xx. It must not be collapsed into an opaque network failure.

## Result states

Every requested engine/mode produces exactly one terminal state:

- `success`: the common scenario completed and all required evidence is valid;
- `unsupported`: the exact requested capability is not implemented by the adapter/browser;
- `unavailable`: the adapter exists but the required binary/channel is absent on the host;
- `timeout`: the bounded case exceeded its declared timeout;
- `error`: execution failed for another recorded reason;
- `invalid`: evidence is incomplete or violates the comparison contract.

Unsupported, unavailable, timeout, error, and invalid cases are data, not success. They remain visible in the raw report and make full-set coverage incomplete.

A worker cannot bypass this contract by returning `success` directly. The parent benchmark runner validates canonical identity, adapter, competitor, mode, payload identity, required success evidence, query evidence, and browser process-scope memory evidence before admitting a successful case.

## Common scenario invariants

Playwright and WebDriver cases consume one shared scenario authority for:

- Unicode payload pattern and 1 MiB corpus chunk construction;
- DOM/CSS geometry;
- exact `800x720` inner CSS viewport;
- virtualized slice size;
- deterministic LCG query/scroll sequence;
- setup GC plus 250 ms warmup policy;
- setup/query timing boundaries;
- double-`requestAnimationFrame` completion semantics;
- timeout policy inputs;
- process-tree memory accounting definition;
- harness schema and scenario fingerprint.

W3C window rectangle dimensions are not accepted as viewport evidence by themselves. The harness measures `window.innerWidth` and `window.innerHeight`, iteratively calibrates the outer rectangle when necessary, and rejects the case if exact `800x720` inner dimensions cannot be established.

Adapter-specific launch/control setup is recorded separately from workload timing.

A browser-native DOM layout and Zevryon's deterministic average-advance layout are not the same rendering workload. Results may be published side-by-side, but a leadership metric may combine them only after the metric contract demonstrates equivalence.

## Browser process-scope memory evidence

Browser memory is measured only from processes created after the adapter's control baseline.

- Process identity is `(PID, creation-time)` so PID reuse cannot alias an unrelated process.
- Playwright's already-running driver belongs to the control baseline and is excluded.
- Servo/Ladybird engine launch occurs after the control baseline and is therefore included.
- Linux uses PSS from `smaps_rollup` when available; RSS is the fallback.
- Post-setup resident memory, post-query resident memory, peak browser-scope memory, and process identity receipts are recorded.
- Empty or unidentifiable browser scope makes the memory evidence `invalid`.
- The harness does not fall back to Python worker-tree memory when browser isolation fails.
- Absence of the live process dependency fails closed; pure contract tests remain importable without requiring that dependency.

## Coverage and leadership gates

The report distinguishes requested, planner-executable, available, unavailable, successfully measured, fully measured, and runtime-unsupported competitors. Auxiliary/non-canonical baselines remain separate from the canonical set.

No leadership claim is admitted when a required canonical competitor is silently missing, mislabeled, measured on mismatched comparison identity, or measured under a materially different workload.

Leadership is explicitly two-stage and fail-closed:

1. the evidence gate requires every canonical competitor to complete successfully with matching host platform/architecture, system fingerprint, harness schema, corpus SHA-256 and scenario fingerprint;
2. only a separate metric evaluator may then evaluate the performance leadership rule.

Passing the evidence gate is **not** a leadership result. The registry/coverage layer reports the metric gate as unevaluated and final leadership eligibility as false until the metric evaluator has processed the admitted raw measurements.

The existing M7 performance rule remains authoritative: Zevryon must be first in at least four core efficiency metrics and within 5% of the leader in every remaining core metric before a leadership claim is allowed.

## Canonical implementation sequence

The harness migration sequence is:

1. explicit competitor registry and requested-engine list;
2. auxiliary Chromium separated from branded Chrome/Edge;
3. real WebKit plus branded Chrome/Edge identity receipts;
4. explicit terminal result states;
5. deterministic corpus/scenario/system evidence;
6. PID-reuse-safe browser process-scope authority;
7. Servo and Ladybird exact-binary launch/identity adapters;
8. shared W3C WebDriver transport;
9. shared Playwright/WebDriver scenario and case-executor wiring with exact viewport and browser process-scope evidence;
10. exact-head validation and admission of the integrated executor;
11. real canonical full-set evidence collection;
12. separate metric evaluation and only then any leadership/ranking claim.

Steps 1-9 are implemented in the integrated M7 candidate. Steps 10-12 remain fail-closed until their evidence exists.
