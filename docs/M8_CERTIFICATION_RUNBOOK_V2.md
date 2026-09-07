# M8 canonical certification runbook v2

## Status

This is the operator order for one final no-compensation M8 certification bundle.

Do **not** start a production certification bundle until the final evidence-binder v2 authority is admitted on canonical `main`. The physical profile collector, canonical Titan authority, storage crash authority, mixed-mutation authority, continuous-soak authority and four-domain property-fuzz authority are already implementation authorities; real long/physical evidence remains separate.

A short CI smoke is never certification evidence.

## Non-negotiable rules

- Use one exact clean Git commit/tree for the entire bundle.
- Use a fresh external artifact root for every independent attempt.
- Never delete a failed artifact/receipt and rerun that slot in the same bundle.
- Never mix successful axes from different bundles.
- Keep build output outside the repository or in an ignored build directory.
- Do not rebuild or modify the candidate after bundle freeze.
- During the 24-hour soak, keep the host dedicated to the soak workload.
- Fresh-process crash evidence is not physical power-loss evidence.

## 1. Pin a clean candidate

```powershell
git status --porcelain=v1 --untracked-files=all
$Commit = (git rev-parse HEAD).Trim()
$Tree   = (git rev-parse 'HEAD^{tree}').Trim()
$Commit
$Tree
```

`git status` must emit nothing.

## 2. Create a fresh Release build

```powershell
$Repo  = (Get-Location).Path
$Build = "C:\Zevryon-M8-Build\$Commit"

if (Test-Path $Build) { throw "Refusing to reuse build directory: $Build" }

cmake -S $Repo -B $Build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build $Build --target `
    zevryon-m8-profile-case-probe `
    zevryon-m8-storage-crash-probe `
    zevryon-m8-mixed-mutation-probe `
    zevryon-m8-continuous-soak-probe `
    zevryon-m8-property-fuzz-probe

ctest --test-dir $Build --output-on-failure -R '^m8-'
if ($LASTEXITCODE -ne 0) { throw "M8 pre-certification smoke failed" }

if (git status --porcelain=v1 --untracked-files=all) {
    throw "Repository became dirty during build/smoke"
}

$ProfileProbe = Join-Path $Build 'zevryon-m8-profile-case-probe.exe'
$StorageProbe = Join-Path $Build 'zevryon-m8-storage-crash-probe.exe'
$MixedProbe   = Join-Path $Build 'zevryon-m8-mixed-mutation-probe.exe'
$SoakProbe    = Join-Path $Build 'zevryon-m8-continuous-soak-probe.exe'
$FuzzProbe    = Join-Path $Build 'zevryon-m8-property-fuzz-probe.exe'
```

Use the corresponding executable names without `.exe` on Linux.

## 3. Generate one canonical certification Titan

Use fresh create-only paths:

```powershell
$TitanRoot   = "C:\Zevryon-M8-Titan\$Commit"
$Titan       = Join-Path $TitanRoot 'titan.zmdoc'
$TitanReport = Join-Path $TitanRoot 'titan-report.json'

if (Test-Path $TitanRoot) { throw "Titan root already exists: $TitanRoot" }
New-Item -ItemType Directory -Path $TitanRoot | Out-Null

python scripts/m8_titan_fixture.py `
    --certification `
    --output $Titan `
    --report $TitanReport

if ($LASTEXITCODE -ne 0) { throw "Canonical Titan generation failed" }
```

The report must bind the same `$Commit`/`$Tree` and certification mode. Do not edit the Titan or report after generation.

## 4. Collect the four physical profile attempts

Each named profile must run on a physical host that independently qualifies for that exact M0 device class. Do not simulate `legacy-phone` by setting an environment variable on a desktop.

Run the same candidate, Titan and profile probe on each qualifying host:

```powershell
python scripts/m8_profile_case_collector.py `
    --probe $ProfileProbe `
    --profile legacy-phone `
    --titan $Titan `
    --titan-report $TitanReport `
    --attempt-dir 'D:\M8-Profile-Attempts\legacy-phone'
```

Repeat on the correct qualifying hosts for:

- `mid-phone`;
- `modern-phone`;
- `desktop`.

Each attempt directory is create-only and preserves at least the physical-host receipt, raw profile case, provenance receipt and probe work/copy output. Preserve failed attempts. Do not replace them in place.

## 5. Freeze one final bundle

Only after the exact candidate contains every admitted M8 authority:

```powershell
$Bundle = "C:\Zevryon-M8-Evidence\$Commit"
if (Test-Path $Bundle) { throw "Bundle already exists: $Bundle" }

python scripts/m8_freeze_bundle.py --artifact-root $Bundle
if ($LASTEXITCODE -ne 0) { throw "Bundle freeze failed" }
```

`bundle-plan.json` now freezes candidate identity, artifact/receipt paths and authority-source hashes. Changing tracked source after this point invalidates the bundle.

## 6. Import and independently verify the four profile attempts

The v2 importer accepts attempt directories, not a hand-authored observations JSON:

```powershell
python scripts/m8_bundle_import_profile.py `
    --artifact-root $Bundle `
    --titan-report $TitanReport `
    --titan $Titan `
    --probe $ProfileProbe `
    --attempt 'legacy-phone=D:\M8-Profile-Attempts\legacy-phone' `
    --attempt 'mid-phone=D:\M8-Profile-Attempts\mid-phone' `
    --attempt 'modern-phone=D:\M8-Profile-Attempts\modern-phone' `
    --attempt 'desktop=D:\M8-Profile-Attempts\desktop'

$ProfileExit = $LASTEXITCODE
if ($ProfileExit -eq 1) { throw "Profile evidence is invalid; preserve bundle" }
if ($ProfileExit -eq 2) { throw "Profile evidence is valid but a profile gate failed; preserve bundle" }
if ($ProfileExit -ne 0) { throw "Unexpected profile importer exit: $ProfileExit" }
```

The importer reruns the independent provenance verifier four times, then recomputes the four-profile collection. It seals small raw support receipts under `profile-support/`. The 4+ GiB Titan and full-copy outputs are not duplicated; their hashes are bound in the import receipt and revalidated by the final binder.

## 7. Collect fresh-process storage crash evidence

```powershell
python scripts/m8_bundle_run.py `
    --artifact-root $Bundle `
    --artifact-key storage_crash `
    -- `
    python scripts/m8_storage_process_crash_tests.py `
        --probe $StorageProbe `
        --work-dir '{artifact_root}\work\storage-crash' `
        --output '{artifact}'

if ($LASTEXITCODE -ne 0) { throw "Storage crash certification failed; preserve bundle" }
```

This certifies abrupt fresh-process recovery across the frozen cut matrix. It does not certify physical power loss.

## 8. Execute at least 10,000,000 mixed mutations

```powershell
python scripts/m8_bundle_run.py `
    --artifact-root $Bundle `
    --artifact-key mixed_mutation `
    -- `
    $MixedProbe `
        --certification `
        --operations 10000000 `
        --output '{artifact}'

if ($LASTEXITCODE -ne 0) { throw "Mixed-mutation certification failed; preserve bundle" }
```

## 9. Execute at least 10,000 property cases per domain

```powershell
python scripts/m8_bundle_run.py `
    --artifact-root $Bundle `
    --artifact-key property_fuzz `
    -- `
    $FuzzProbe `
        --certification `
        --cases 10000 `
        --work-dir '{artifact_root}\work\property-fuzz' `
        --output '{artifact}'

if ($LASTEXITCODE -ne 0) { throw "Property-fuzz certification failed; preserve bundle" }
```

The exact domains are Unicode, serializer, index and sequence. All four must pass independently.

## 10. Execute the continuous 24-hour soak

Run this only when the host can remain dedicated for the complete measured interval:

```powershell
python scripts/m8_bundle_run.py `
    --artifact-root $Bundle `
    --artifact-key soak `
    -- `
    $SoakProbe `
        --certification `
        --duration-seconds 86400 `
        --work-dir '{artifact_root}\work\continuous-soak' `
        --output '{artifact}'

if ($LASTEXITCODE -ne 0) { throw "24-hour soak failed; preserve bundle" }
```

Do not suspend/hibernate, rebuild, modify tracked files or intentionally load the host while this axis is running.

## 11. Seal the final decision

Only after all five artifact slots and their receipts exist:

```powershell
python scripts/m8_final_evidence_binder.py --artifact-root $Bundle
$BinderExit = $LASTEXITCODE

Get-Content (Join-Path $Bundle 'final-certification.json')

if ($BinderExit -eq 1) { throw "M8 bundle is evidence-invalid" }
if ($BinderExit -eq 2) { throw "M8 evidence is valid but at least one gate failed" }
if ($BinderExit -ne 0) { throw "Unexpected binder exit: $BinderExit" }
```

Exit `0` is the only M8 PASS. `final-certification.json` is create-only.

## 12. Preserve publication evidence

Keep together:

- `bundle-plan.json`;
- all five raw artifacts;
- all five top-level receipts;
- the complete `profile-support/` tree;
- `final-certification.json`;
- the canonical Titan report and the exact Titan/probe/copy external artifacts referenced by hashes;
- exact candidate commit/tree and build identity;
- all failed independent bundles/attempts as failure evidence.

No axis from another bundle may replace an axis in this one.

## M7 remains separate

Passing M8 does not create M7 competitor leadership evidence. M7 still requires the real physical six-runtime readiness preflight, canonical 6x2 collection, physical Zevryon cases, v2 admission replay, evaluator and publication manifest.