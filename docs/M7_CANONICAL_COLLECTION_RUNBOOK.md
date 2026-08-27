# M7 canonical normalized collection runbook

## Purpose

This runbook describes the evidence sequence for the M7 giant-document leadership gate. It is intentionally separate from ordinary CI. CI validates implementation integrity; this sequence collects real runtime evidence.

Do not substitute an unavailable canonical runtime with a different engine. In particular:

- Chrome means the exact branded Chrome channel;
- Edge means the exact branded Edge channel;
- WebKit means the declared Playwright WebKit runtime;
- Servo means the exact Servo binary selected by the Servo adapter;
- Ladybird means the exact Ladybird WebDriver binary selected by the Ladybird adapter;
- bundled Chromium is not canonical Chrome or Edge.

The fixed canonical competitor set is Chrome, Firefox, Edge, WebKit, Servo and Ladybird. Every competitor must succeed in both `virtualized` and `native-dom` modes before metric evaluation is admissible.

## 1. Build the Zevryon persistent-session executable

Build the repository normally with testing enabled. The required native executable is:

`zevryon-massivedoc-benchmark-session`

On Windows the executable normally has the `.exe` suffix. Record the exact path used for evidence collection.

The normalized Zevryon collector hashes this executable into its runtime identity.

## 2. Configure exact Servo and Ladybird runtimes

When auto-discovery is not sufficient, use the explicit environment authorities already supported by the adapters:

- `ZEVRYON_SERVO_BIN`
- `ZEVRYON_LADYBIRD_WEBDRIVER_BIN`

Do not point either variable at a wrapper that silently launches another engine.

Chrome and Edge use their exact branded Playwright channels. Firefox and WebKit use their declared Playwright distributions. A missing distribution remains `unavailable`; it is not silently replaced.

## 3. Run runtime preflight as a separate readiness stage

Run:

```text
python scripts/m7_runtime_preflight.py --output evidence/m7/runtime-preflight.json
```

A passing preflight proves that all six exact canonical runtimes can be launched/readiness-checked and closed on the recorded host/system fingerprint.

Preflight is **not** benchmark measurement. Its report must retain:

- `measurement_started: false`;
- exact runtime identities;
- host metadata;
- system fingerprint;
- explicit unavailable/error reasons when a runtime cannot be admitted.

Do not start canonical timing collection merely because the script exited. Inspect `preflight_gate_passed` and preserve the JSON artifact.

### Cache/state note

Runtime preflight may warm executable/page-cache state. It is therefore deliberately not embedded immediately inside the timed collection command. Use the repository's declared benchmark system-state discipline before collecting timed evidence. Do not selectively warm one implementation while another pays cold runtime/source costs.

The later collection-admission binder checks stable runtime identity so the preflight and measurement cannot silently use different engine builds. Ephemeral WebDriver ports may differ and are normalized only for that identity comparison.

## 4. Collect normalized Zevryon virtualized evidence

Use the same payload/query/warmup/slice/timeout authority that will be used for the browser collection. With canonical defaults:

```text
python scripts/m7_zevryon_normalized_case.py \
  --session-binary <path-to-zevryon-massivedoc-benchmark-session> \
  --mode virtualized \
  --timeout-seconds 180 \
  --output evidence/m7/zevryon-virtualized.json
```

The Zevryon process constructs the exact single-record M7 synthetic store after process launch. Store construction, required preparation and warmups are charged to setup. The collector rejects a prebuilt-store authority.

## 5. Collect normalized Zevryon native-DOM evidence

```text
python scripts/m7_zevryon_normalized_case.py \
  --session-binary <path-to-zevryon-massivedoc-benchmark-session> \
  --mode native-dom \
  --timeout-seconds 420 \
  --output evidence/m7/zevryon-native-dom.json
```

Native-DOM checkpoint/layout preparation occurs inside the case-owned lifecycle and is charged to setup.

## 6. Collect the exact canonical browser 6x2 full set

Run the legacy-independent collector:

```text
python scripts/m7_normalized_browser_full_set.py \
  --output evidence/m7/browser-full-set.json
```

The canonical defaults are:

- payload: 64 MiB exact M7 synthetic corpus;
- measured queries: 21;
- warmups: the fixed `DEFAULT_WARMUP_QUERY_COUNT` authority;
- virtualized slice: 128 KiB;
- virtualized timeout: 180 seconds;
- native-DOM timeout: 420 seconds.

If any canonical runtime/case is unavailable, unsupported, invalid, times out or errors, the collector preserves that terminal record but the full-set gate fails. Do not delete failed records and do not recast missing evidence as zero.

This collector does not require the legacy `zevryon.massivedoc.benchmark.v4` report. Legacy giant-document summaries remain diagnostics only.

## 7. Bind and admit the evidence bundle

After all four evidence artifacts exist, run:

```text
python scripts/m7_collection_admission.py \
  --preflight evidence/m7/runtime-preflight.json \
  --browser-report evidence/m7/browser-full-set.json \
  --zevryon-virtualized evidence/m7/zevryon-virtualized.json \
  --zevryon-native-dom evidence/m7/zevryon-native-dom.json \
  --output evidence/m7/collection-admission.json
```

Admission revalidates:

- preflight host/system fingerprint;
- exact six-runtime identity binding;
- both browser modes for every canonical competitor;
- deterministic M7 corpus identity;
- Zevryon persistent-session transcript and case-owned store authority;
- normalized setup/query/memory evidence;
- cross-implementation comparability;
- the fixed five-metric leadership rule.

The admission artifact also records SHA-256 hashes of the four input evidence files.

## Exit-code semantics

Treat exit codes deliberately:

- `0`: the stage passed; for `m7_collection_admission.py`, the complete evidence bundle is leadership-eligible;
- `1`: invalid/incomplete/unavailable evidence or harness/runtime failure; do not claim leadership;
- `2`: **the evidence bundle is valid and the five-metric gate was evaluated, but Zevryon did not satisfy the fixed leadership threshold**.

Exit code `2` is not an infrastructure failure and is not a reason to rerun identical evidence until a nicer number appears.

## Fixed leadership rule

All five metrics are lower-is-better:

1. `setup_to_ready_seconds`;
2. `query_milliseconds_p50`;
3. `query_milliseconds_p95`;
4. `query_milliseconds_p99`;
5. `incremental_peak_memory_mb`.

For each canonical mode independently, Zevryon must:

- be first in at least four of five metrics; and
- on every metric where it is not first, remain within 5% of the leader.

Exact ties count as first. Overall M7 leadership eligibility requires both `virtualized` and `native-dom` mode gates to pass.

## Evidence hygiene

Preserve the original JSON artifacts unchanged after collection admission. Do not hand-edit runtime identities, timings, query samples, corpus hashes, coverage summaries or status fields.

If a new binary, browser/engine version, timeout, warmup count, query count, payload, slice size or other scenario-semantic parameter is introduced, collect a new internally consistent evidence set. Do not combine records from incompatible scenario fingerprints.

A successful unit test, preflight, build or GitHub Actions run is not by itself a performance leadership claim.
