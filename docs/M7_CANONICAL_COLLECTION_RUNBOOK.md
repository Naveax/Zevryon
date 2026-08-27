# M7 canonical normalized collection runbook

## Purpose

This runbook describes the evidence sequence for the M7 giant-document leadership gate. It is intentionally separate from ordinary CI. CI validates implementation integrity; this sequence collects real runtime evidence.

Do not substitute an unavailable canonical runtime with a different engine. Chrome means the exact branded Chrome channel, Edge means the exact branded Edge channel, WebKit means the declared Playwright WebKit runtime, Servo means the exact Servo binary selected by its adapter, and Ladybird means the exact Ladybird WebDriver binary. Bundled Chromium is not canonical Chrome or Edge.

The fixed canonical competitor set is Chrome, Firefox, Edge, WebKit, Servo and Ladybird. Every competitor must succeed in both `virtualized` and `native-dom` modes before metric evaluation is admissible.

## 1. Build the Zevryon persistent-session executable

Build the repository normally with testing enabled. The required native executable is `zevryon-massivedoc-benchmark-session`; on Windows it normally has the `.exe` suffix. Record the exact path used for evidence collection. The normalized Zevryon collector hashes this executable into its runtime identity.

## 2. Configure exact Servo and Ladybird runtimes

When auto-discovery is not sufficient, use:

- `ZEVRYON_SERVO_BIN`
- `ZEVRYON_LADYBIRD_WEBDRIVER_BIN`

Do not point either variable at a wrapper that silently launches another engine. Chrome and Edge use their exact branded Playwright channels. Firefox and WebKit use their declared Playwright distributions. A missing distribution remains `unavailable`; it is not silently replaced.

## 3. Establish physical-host and thermal evidence

Canonical M7 leadership evidence must satisfy the existing M0 physical benchmark certification rule. An ordinary CI runner, VM, container or unconfirmed workstation must not silently become leadership evidence.

Set physical confirmation only when collection really runs on the intended physical benchmark machine:

```text
ZEVRYON_PHYSICAL_DEVICE=1
```

PowerShell example:

```powershell
$env:ZEVRYON_PHYSICAL_DEVICE = '1'
$env:ZEVRYON_BENCHMARK_RUN_LABEL = 'm7-canonical'
```

A thermal observation is also mandatory. Linux may provide raw readings through sysfs automatically. Where no authoritative automatic source exists, supply an observation from the actual lab/sensor state:

```powershell
$env:ZEVRYON_THERMAL_C = '<observed-celsius-value>'
```

An authoritative qualitative state may be supplied as:

```powershell
$env:ZEVRYON_THERMAL_STATE = '<nominal|fair|serious|critical|unknown>'
```

Do not invent `nominal`, a temperature, or any other thermal value merely to make the gate pass. Unknown/unobserved thermal state is valid diagnostic metadata but is not sufficient for physical leadership certification.

M7 host metadata embeds the existing M0 `BenchmarkMachineMetadata` receipt. Its stable system fingerprint uses M0 machine identity fields while dynamic capture time, run label and thermal observations remain raw evidence outside the stable hash. Refresh any manually supplied thermal observation immediately before each physical collection boundary.

## 4. Run runtime preflight as a separate readiness stage

Run:

```text
python scripts/m7_runtime_preflight.py --output evidence/m7/runtime-preflight.json
```

A passing preflight proves that all six exact canonical runtimes can be launched/readiness-checked and closed on the recorded host/system fingerprint. Preflight is **not** benchmark measurement. Preserve `measurement_started: false`, exact runtime identities, stable host/system identity, the nested M0 physical/thermal receipt, and explicit unavailable/error reasons.

Do not start canonical timing collection merely because the script exited. Inspect `preflight_gate_passed` and preserve the JSON artifact.

### Cache/state note

Runtime preflight may warm executable/page-cache state. It is deliberately separate from timed collection. Follow the declared benchmark system-state discipline and do not selectively warm one implementation while another pays cold runtime/source costs. Collection admission later binds stable runtime identities; only ephemeral Servo/Ladybird WebDriver ports are normalized away.

## 5. Collect canonical physical Zevryon virtualized evidence

Use the physical wrapper rather than the lower-level normalized collector directly:

```text
python scripts/m7_zevryon_physical_case.py \
  --session-binary <path-to-zevryon-massivedoc-benchmark-session> \
  --mode virtualized \
  --timeout-seconds 180 \
  --output evidence/m7/zevryon-virtualized.json
```

The wrapper captures and certifies M0 machine/thermal evidence immediately before and after the normalized timed case. Both receipts must map to the same stable system fingerprint as the normalized result. Missing physical confirmation, missing thermal observation or before/after machine-identity drift fails closed.

The underlying process constructs the exact single-record M7 synthetic store after process launch. Store construction, required preparation and warmups are charged to setup. A prebuilt Zevryon store is not normalized leadership evidence.

## 6. Collect canonical physical Zevryon native-DOM evidence

Refresh any manually supplied thermal observation, then run:

```text
python scripts/m7_zevryon_physical_case.py \
  --session-binary <path-to-zevryon-massivedoc-benchmark-session> \
  --mode native-dom \
  --timeout-seconds 420 \
  --output evidence/m7/zevryon-native-dom.json
```

Native-DOM checkpoint/layout preparation occurs inside the case-owned lifecycle and is charged to setup. Both before/after physical-host receipts are mandatory leadership evidence.

## 7. Collect the exact canonical browser 6x2 full set

Refresh a manually supplied thermal observation immediately before this stage when applicable, then run:

```text
python scripts/m7_normalized_browser_full_set.py \
  --output evidence/m7/browser-full-set.json
```

Canonical defaults are 64 MiB exact M7 synthetic corpus, 21 measured queries, fixed `DEFAULT_WARMUP_QUERY_COUNT`, 128 KiB virtualized slice, 180 second virtualized timeout and 420 second native-DOM timeout.

If any canonical runtime/case is unavailable, unsupported, invalid, times out or errors, preserve that terminal record. The full-set gate fails. Do not delete failed records or recast missing evidence as zero. Legacy giant-document summaries remain diagnostic only.

## 8. Bind and admit the evidence bundle

Run:

```text
python scripts/m7_collection_admission.py \
  --preflight evidence/m7/runtime-preflight.json \
  --browser-report evidence/m7/browser-full-set.json \
  --zevryon-virtualized evidence/m7/zevryon-virtualized.json \
  --zevryon-native-dom evidence/m7/zevryon-native-dom.json \
  --output evidence/m7/collection-admission.json
```

Admission revalidates:

- stable preflight/browser system fingerprint;
- nested M0 machine receipts against top-level stable identities;
- explicit physical-device confirmation and observed thermal evidence;
- before/after M0 physical-host receipts for both Zevryon modes;
- exact six-runtime identity binding;
- both browser modes for every canonical competitor;
- deterministic M7 corpus identity;
- Zevryon persistent-session and case-owned-store authority;
- normalized setup/query/memory evidence;
- cross-implementation comparability;
- the fixed five-metric leadership rule.

The admission artifact records physical-host certification receipts and SHA-256 hashes of all four raw evidence files.

## 9. Create the canonical publication manifest

Run this from the exact admitted Git checkout used for collection. Tracked source files must still match `HEAD`; untracked evidence output files are allowed.

```text
python scripts/m7_evidence_bundle_manifest.py \
  --admission evidence/m7/collection-admission.json \
  --artifact-root . \
  --repo-root . \
  --expected-tool-commit <admitted-40-hex-commit> \
  --output evidence/m7/evidence-bundle-manifest.json
```

Publication does **not** trust `collection-admission.json` merely because it has the right schema or SHA. Before writing the manifest it:

1. resolves every declared raw evidence path under `artifact_root`;
2. rejects relative traversal, absolute out-of-root paths and any other resolved path escape before reading the artifact;
3. re-hashes all four raw evidence artifacts and checks the admission receipts;
4. re-reads runtime preflight, browser full-set and both physical Zevryon artifacts;
5. reruns `admit_collection()` from those four raw objects;
6. requires the stored admission JSON to match the recomputed admission field-for-field except for its explicit `input_artifacts` receipts;
7. emits an admission-replay receipt containing the exact four raw artifact SHA-256 values;
8. requires those replay hashes to equal the publication verifier's independently checked artifact hashes.

A hand-edited or forged admission JSON therefore cannot become publishable simply by recomputing its file SHA.

The publication manifest then binds:

- SHA-256 of `collection-admission.json`;
- SHA-256 of runtime preflight, browser full-set and both Zevryon raw artifacts;
- the admission-replay receipt;
- exact Git `HEAD` commit and tree;
- a clean tracked worktree relative to `HEAD`;
- system fingerprint and corpus SHA-256;
- preflight/browser and both Zevryon before/after physical-host/thermal receipts;
- all six stable runtime bindings;
- the complete five-metric evaluation and final eligibility result.

The manifest has its own canonical `manifest_payload_sha256`. Any later field mutation without a new valid manifest is rejected.

A valid bundle that does **not** satisfy the leadership threshold is still publishable evidence. It uses `result_class: valid_not_leadership`. Preserve that result rather than rerunning unchanged evidence until a preferred ranking appears.

## Exit-code semantics

- `0`: stage passed. For collection admission this means leadership-eligible; for publication it means the already-admitted result was replayed, verified and packaged regardless of leadership true/false.
- `1`: invalid/incomplete/unavailable evidence, missing physical/thermal certification, artifact-root escape, replay mismatch, source-tree mismatch, artifact hash mismatch or harness/runtime failure. Do not claim leadership.
- `2`: for collection admission, the bundle is valid and the five-metric gate was evaluated, but Zevryon did not satisfy the fixed leadership threshold.

Exit code `2` is not infrastructure failure and is not a reason to rerun identical evidence until a nicer number appears.

## Fixed leadership rule

All five metrics are lower-is-better:

1. `setup_to_ready_seconds`
2. `query_milliseconds_p50`
3. `query_milliseconds_p95`
4. `query_milliseconds_p99`
5. `incremental_peak_memory_mb`

For each canonical mode independently, Zevryon must be first in at least four of five metrics and be within 5% of the leader in every metric where it is not first. Exact ties count as first. Overall eligibility requires both `virtualized` and `native-dom` mode gates to pass.

## Evidence hygiene

Preserve original JSON artifacts unchanged after collection admission and publication. Do not hand-edit runtime identities, timings, query samples, corpus hashes, host receipts, thermal observations, coverage summaries, admission fields, replay receipts or publication receipts.

If a binary, browser/engine version, timeout, warmup count, query count, payload, slice size or other scenario-semantic parameter changes, collect a new internally consistent evidence set. Do not combine incompatible scenario fingerprints.

A successful unit test, preflight, build or GitHub Actions run is not by itself a performance leadership claim.
