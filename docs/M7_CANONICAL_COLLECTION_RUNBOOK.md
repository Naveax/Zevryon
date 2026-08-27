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

On Windows the executable normally has the `.exe` suffix. Record the exact path used for evidence collection. The normalized Zevryon collector hashes this executable into its runtime identity.

## 2. Configure exact Servo and Ladybird runtimes

When auto-discovery is not sufficient, use the explicit environment authorities already supported by the adapters:

- `ZEVRYON_SERVO_BIN`
- `ZEVRYON_LADYBIRD_WEBDRIVER_BIN`

Do not point either variable at a wrapper that silently launches another engine. Chrome and Edge use their exact branded Playwright channels. Firefox and WebKit use their declared Playwright distributions. A missing distribution remains `unavailable`; it is not silently replaced.

## 3. Establish physical-host and thermal evidence

Canonical M7 leadership evidence must satisfy the existing M0 physical benchmark certification rule. An ordinary CI runner, VM, container or unconfirmed workstation must not silently become leadership evidence.

Set physical confirmation only when collection is really being performed on the intended physical benchmark machine:

```text
ZEVRYON_PHYSICAL_DEVICE=1
```

On PowerShell:

```powershell
$env:ZEVRYON_PHYSICAL_DEVICE = '1'
$env:ZEVRYON_BENCHMARK_RUN_LABEL = 'm7-canonical'
```

A thermal observation is also mandatory. Linux may provide raw readings through sysfs automatically. On hosts where no authoritative automatic source is available, supply an observation from the actual lab/sensor state. Replace the placeholder with the value actually observed at that stage:

```powershell
$env:ZEVRYON_THERMAL_C = '<observed-celsius-value>'
```

If an authoritative qualitative state exists, it may be supplied as:

```powershell
$env:ZEVRYON_THERMAL_STATE = '<nominal|fair|serious|critical|unknown>'
```

Do not invent `nominal`, a temperature, or any other thermal value merely to make the gate pass. Unknown/unobserved thermal state is valid diagnostic metadata but is not sufficient for physical leadership certification.

The M7 host metadata embeds the existing M0 `BenchmarkMachineMetadata` receipt. Its stable system fingerprint uses the M0 machine identity fields while dynamic capture time, run label and thermal values remain raw evidence outside the stable hash.

Refresh any manually supplied thermal observation immediately before each physical collection boundary so the embedded receipt describes the actual state.

## 4. Run runtime preflight as a separate readiness stage

Run:

```text
python scripts/m7_runtime_preflight.py --output evidence/m7/runtime-preflight.json
```

A passing preflight proves that all six exact canonical runtimes can be launched/readiness-checked and closed on the recorded host/system fingerprint.

Preflight is **not** benchmark measurement. Its report must retain:

- `measurement_started: false`;
- exact runtime identities;
- stable host/system identity;
- the nested M0 physical/thermal machine receipt;
- explicit unavailable/error reasons when a runtime cannot be admitted.

Do not start canonical timing collection merely because the script exited. Inspect `preflight_gate_passed` and preserve the JSON artifact.

### Cache/state note

Runtime preflight may warm executable/page-cache state. It is therefore deliberately not embedded immediately inside the timed collection command. Use the repository's declared benchmark system-state discipline before collecting timed evidence. Do not selectively warm one implementation while another pays cold runtime/source costs.

The later collection-admission binder checks stable runtime identity so the preflight and measurement cannot silently use different engine builds. Ephemeral WebDriver ports may differ and are normalized only for that identity comparison.

## 5. Collect canonical physical Zevryon virtualized evidence

Use the physical wrapper, not the lower-level normalized collector directly:

```text
python scripts/m7_zevryon_physical_case.py \
  --session-binary <path-to-zevryon-massivedoc-benchmark-session> \
  --mode virtualized \
  --timeout-seconds 180 \
  --output evidence/m7/zevryon-virtualized.json
```

The wrapper captures and certifies M0 machine/thermal evidence immediately before and after the normalized timed case. Both raw receipts must map to the same stable system fingerprint as the normalized result. Missing physical confirmation, missing thermal observation, or a before/after machine-identity drift fails closed.

The underlying Zevryon process constructs the exact single-record M7 synthetic store after process launch. Store construction, required preparation and warmups are charged to setup. The collector rejects a prebuilt-store authority.

## 6. Collect canonical physical Zevryon native-DOM evidence

Refresh any manually supplied thermal observation, then run:

```text
python scripts/m7_zevryon_physical_case.py \
  --session-binary <path-to-zevryon-massivedoc-benchmark-session> \
  --mode native-dom \
  --timeout-seconds 420 \
  --output evidence/m7/zevryon-native-dom.json
```

Native-DOM checkpoint/layout preparation occurs inside the case-owned lifecycle and is charged to setup. As with the virtualized case, both before/after physical-host receipts are mandatory leadership evidence.

## 7. Collect the exact canonical physical browser 6x2 full set

Refresh a manually supplied thermal observation immediately before this stage when applicable, then use the physical wrapper rather than the lower-level normalized collector directly:

```text
python scripts/m7_physical_browser_full_set.py \
  --output evidence/m7/browser-full-set.json
```

The wrapper captures and certifies M0 machine/thermal evidence immediately before and immediately after the complete 6x2 browser stage. Both receipts must retain the same stable system fingerprint as the normalized browser report. Missing physical confirmation, missing thermal observation, post-stage machine drift or forged embedded physical receipts fail closed.

The stage-level before/after design is deliberate. A manually supplied Windows thermal environment value is not a live per-case sensor; duplicating one static override into 24 synthetic per-case snapshots would create false precision rather than stronger evidence. A future per-case thermal contract requires an actual live provider or another precommitted observation authority.

The canonical defaults are:

- payload: 64 MiB exact M7 synthetic corpus;
- measured queries: 21;
- warmups: the fixed `DEFAULT_WARMUP_QUERY_COUNT` authority;
- virtualized slice: 128 KiB;
- virtualized timeout: 180 seconds;
- native-DOM timeout: 420 seconds.

If any canonical runtime/case is unavailable, unsupported, invalid, times out or errors, the underlying collector preserves that terminal record but the full-set gate fails. Do not delete failed records and do not recast missing evidence as zero.

The physical wrapper remains independent of the legacy `zevryon.massivedoc.benchmark.v4` report. Legacy giant-document summaries remain diagnostics only.

## 8. Bind and admit the evidence bundle with atomic artifact snapshots

After all four evidence artifacts exist, run:

```text
python scripts/m7_collection_admission_v3.py \
  --preflight evidence/m7/runtime-preflight.json \
  --browser-report evidence/m7/browser-full-set.json \
  --zevryon-virtualized evidence/m7/zevryon-virtualized.json \
  --zevryon-native-dom evidence/m7/zevryon-native-dom.json \
  --output evidence/m7/collection-admission.json
```

Admission v3 reads each raw JSON input exactly once. The SHA-256 receipt, byte count, UTF-8 decoding and JSON object used for admission all come from that same byte snapshot. The admission therefore does not hash one version of a file and parse another version through a separate I/O call.

Admission revalidates:

- stable preflight/browser host system fingerprint;
- nested M0 machine receipts against their top-level stable identities;
- explicit physical-device confirmation;
- observed thermal evidence at preflight;
- browser full-set before/after physical-host and thermal receipts;
- before/after M0 physical-host and thermal receipts for both Zevryon modes;
- exact six-runtime identity binding;
- both browser modes for every canonical competitor;
- deterministic M7 corpus identity;
- Zevryon persistent-session transcript and case-owned store authority;
- normalized setup/query/memory evidence;
- cross-implementation comparability;
- the fixed five-metric leadership rule.

The canonical admission contract is schema `zevryon.competitor.collection-admission.v3`. Each raw input receipt records `path`, `sha256` and `byte_count` from the exact atomic byte snapshot used by the admission calculation.

## 9. Create the canonical v3 publication manifest

Run this from the exact admitted Git checkout used for collection. Tracked source files must still match `HEAD`; untracked evidence output files are allowed.

```text
python scripts/m7_evidence_bundle_manifest_v3.py \
  --admission evidence/m7/collection-admission.json \
  --artifact-root . \
  --repo-root . \
  --expected-tool-commit <admitted-40-hex-commit> \
  --output evidence/m7/evidence-bundle-manifest.json
```

Publication does not trust `collection-admission.json` merely because it has the expected schema or a matching file hash. The v3 publisher performs this sequence:

1. constrain the admission artifact under `artifact_root` and read it once as an atomic byte snapshot;
2. constrain all four declared raw evidence paths under `artifact_root`;
3. read each raw artifact once, deriving SHA-256, byte count and parsed JSON from the same bytes;
4. rerun the v3 `admit_collection()` authority from those four raw objects;
5. require the stored admission JSON to equal the recomputed admission field-for-field apart from its explicit raw-artifact receipts;
6. calculate and record a canonical SHA-256 over the recomputed admission authority fields themselves;
7. require the replay receipt's raw SHA/byte-count set to equal the stored admission receipts;
8. verify exact Git `HEAD`, exact tree and a clean tracked worktree.

The publication manifest then binds:

- SHA-256 and byte count of `collection-admission.json` from its atomic snapshot;
- SHA-256 and byte count of runtime preflight, physical browser full-set and both physical Zevryon raw artifacts;
- the raw-artifact admission-replay receipt;
- the canonical recomputed-admission core SHA-256;
- exact Git `HEAD` commit and tree;
- a clean tracked worktree relative to `HEAD`;
- system fingerprint and corpus SHA-256;
- preflight physical receipt, browser full-set before/after receipts and both Zevryon before/after receipts;
- all six stable runtime bindings;
- the complete five-metric evaluation and final eligibility result.

The canonical publication contract is schema `zevryon.competitor.evidence-bundle-manifest.v3`. The manifest has its own canonical `manifest_payload_sha256`. Manifest validation reconstructs the bound v3 admission core from the manifest fields and requires it to reproduce the replay/admission core SHA before accepting the manifest payload hash.

A valid bundle that does **not** satisfy the leadership threshold is still publishable evidence. Its manifest uses `result_class: valid_not_leadership`. Publication must preserve that result rather than rerun unchanged evidence until a preferred ranking appears.

## Exit-code semantics

Treat exit codes deliberately:

- `0`: the stage passed; for `m7_collection_admission_v3.py`, the complete evidence bundle is leadership-eligible; for `m7_evidence_bundle_manifest_v3.py`, the already-admitted result was replayed, verified and packaged regardless of whether leadership is true or false;
- `1`: invalid/incomplete/unavailable evidence, missing physical/thermal certification, artifact-root escape, admission-replay mismatch, admission-core hash mismatch, source-tree mismatch, artifact SHA/byte-count mismatch, or harness/runtime failure; do not claim leadership;
- `2`: for `m7_collection_admission_v3.py`, the evidence bundle is valid and the five-metric gate was evaluated, but Zevryon did not satisfy the fixed leadership threshold.

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

Preserve the original JSON artifacts unchanged after collection admission and publication-manifest generation. Do not hand-edit runtime identities, timings, query samples, corpus hashes, host receipts, thermal observations, coverage summaries, status fields, admission fields, replay receipts or publication receipts.

If a new binary, browser/engine version, timeout, warmup count, query count, payload, slice size or other scenario-semantic parameter is introduced, collect a new internally consistent evidence set. Do not combine records from incompatible scenario fingerprints.

A successful unit test, preflight, build or GitHub Actions run is not by itself a performance leadership claim.
