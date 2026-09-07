# M8 canonical certification runbook

## Status

This runbook is the execution order for one final M8 no-compensation certification bundle.

**Do not freeze or execute a production certification bundle yet.** The raw four-profile evaluator is admitted, but the measurement collector that must produce those observations is not yet admitted. A hand-authored `profile-observations.json` is not certification evidence.

The commands below become executable only after:

1. the property-fuzz authority is admitted on canonical `main`;
2. the final bundle/binder authority is admitted on canonical `main`;
3. the physical/raw four-profile measurement collector is admitted on the same candidate lineage;
4. the exact candidate chosen for certification is clean and contains all of those authorities.

## Non-negotiable evidence rules

- One certification decision uses one pre-frozen bundle and one exact clean Git commit/tree.
- The artifact root lives outside the repository.
- Build/test output also lives outside the repository or under an ignored build directory.
- Every artifact and invocation receipt is create-only.
- Do not delete a failed artifact or receipt and rerun the same slot in the same bundle.
- Do not cherry-pick successful axes from different bundles.
- A short CI smoke is never long-run certification evidence.
- Process-crash evidence is not physical power-loss evidence.
- During the 24-hour soak, do not run another benchmark, compiler job, game, stress test, update job or other avoidable high-load workload on the certification host.

## 1. Pin the candidate

From the repository root:

```powershell
git status --porcelain=v1 --untracked-files=all
git rev-parse HEAD
git rev-parse 'HEAD^{tree}'
```

`git status` must emit nothing.

Record the exact commit shown by `git rev-parse HEAD`. Do not continue on a dirty tree.

## 2. Create a fresh external Release build

The canonical Windows example uses Ninja so executable paths are deterministic and do not depend on a Visual Studio configuration subdirectory.

```powershell
$Repo = (Get-Location).Path
$Commit = (git rev-parse HEAD).Trim()
$Build = "C:\Zevryon-M8-Build\$Commit"

if (Test-Path $Build) {
    throw "Refusing to reuse an existing certification build directory: $Build"
}

cmake -S $Repo -B $Build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build $Build --target `
    zevryon-m8-storage-crash-probe `
    zevryon-m8-mixed-mutation-probe `
    zevryon-m8-continuous-soak-probe `
    zevryon-m8-property-fuzz-probe

ctest --test-dir $Build --output-on-failure -R '^m8-'

if ($LASTEXITCODE -ne 0) {
    throw "M8 pre-certification smoke suite failed"
}

git status --porcelain=v1 --untracked-files=all
```

The repository must still be clean after the external build and smoke suite.

Bind executable paths:

```powershell
$StorageProbe = Join-Path $Build 'zevryon-m8-storage-crash-probe.exe'
$MixedProbe   = Join-Path $Build 'zevryon-m8-mixed-mutation-probe.exe'
$SoakProbe    = Join-Path $Build 'zevryon-m8-continuous-soak-probe.exe'
$FuzzProbe    = Join-Path $Build 'zevryon-m8-property-fuzz-probe.exe'

$StorageProbe, $MixedProbe, $SoakProbe, $FuzzProbe | ForEach-Object {
    if (-not (Test-Path -LiteralPath $_ -PathType Leaf)) {
        throw "Required certification binary is missing: $_"
    }
}
```

On Linux, use the same Ninja configuration and binary names without `.exe`.

## 3. Freeze one empty external bundle

Choose a new path for every independent certification attempt. Never reuse a prior bundle root.

```powershell
$Bundle = "C:\Zevryon-M8-Evidence\$Commit"

if (Test-Path $Bundle) {
    throw "Certification bundle already exists: $Bundle"
}

python scripts/m8_freeze_bundle.py --artifact-root $Bundle
if ($LASTEXITCODE -ne 0) {
    throw "M8 bundle freeze failed"
}
```

`bundle-plan.json` now freezes the bundle id, exact candidate commit/tree, fixed artifact/receipt paths and explicit authority-source hashes. The exact Git tree remains the complete candidate-content authority.

After this point, changing tracked repository content invalidates the bundle.

## 4. Import raw four-profile observations

**Current status: BLOCKED until the admitted raw profile measurement collector exists.**

Do not construct this document by copying target numbers from `performance_contract.py` or test fixtures.

Once the collector is admitted, it must emit one raw document with exactly one measured observation for each profile:

- `legacy-phone`;
- `mid-phone`;
- `modern-phone`;
- `desktop`.

The collector output must bind the same frozen candidate commit/tree.

Canonical import command:

```powershell
$ProfileRaw = 'C:\Zevryon-M8-Profile-Evidence\profile-observations.json'

python scripts/m8_bundle_import_profile.py `
    --artifact-root $Bundle `
    --input $ProfileRaw

if ($LASTEXITCODE -notin @(0, 2)) {
    throw "Profile evidence/import is invalid"
}
if ($LASTEXITCODE -eq 2) {
    throw "Profile evidence is valid but at least one no-compensation gate failed; preserve this bundle"
}
```

Do not continue a production PASS claim after exit `2`. Preserve the failed bundle as evidence.

## 5. Collect fresh-process storage crash evidence

```powershell
python scripts/m8_bundle_run.py `
    --artifact-root $Bundle `
    --artifact-key storage_crash `
    -- `
    python scripts/m8_storage_process_crash_tests.py `
        --probe $StorageProbe `
        --work-dir '{artifact_root}\work\storage-crash' `
        --output '{artifact}'

if ($LASTEXITCODE -ne 0) {
    throw "Storage crash authority failed; preserve this bundle and do not rerun the slot"
}
```

This executes all five publication and three compaction abrupt-exit cuts. It certifies fresh-process recovery only, not physical power loss.

## 6. Execute the >=10,000,000 mixed-mutation run

```powershell
python scripts/m8_bundle_run.py `
    --artifact-root $Bundle `
    --artifact-key mixed_mutation `
    -- `
    $MixedProbe `
        --certification `
        --operations 10000000 `
        --output '{artifact}'

if ($LASTEXITCODE -ne 0) {
    throw "Mixed-mutation certification failed; preserve this bundle and do not rerun the slot"
}
```

The final binder independently requires at least 10,000,000 requested and completed operations, all five mutation classes, nonzero verification checkpoints, equal live/oracle digests and zero integrity/order mismatches.

## 7. Execute >=10,000 property cases in each domain

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

if ($LASTEXITCODE -ne 0) {
    throw "Property-fuzz certification failed; preserve this bundle and do not rerun the slot"
}
```

All exact domains must pass independently: Unicode, serializer, index and sequence.

## 8. Execute the continuous >=86,400-second soak

Run this only when the host can remain dedicated to the certification workload for the complete measured interval.

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

if ($LASTEXITCODE -ne 0) {
    throw "24-hour soak failed; preserve this bundle and do not rerun the slot"
}
```

The measured 86,400-second timer begins only after setup and warmup. Certification mode uses the frozen 60-second checkpoint interval and one-second memory sampling. A continuity gap above twice the interval fails closed.

Do not suspend/hibernate the host, pause the process, change the candidate tree, rebuild binaries, or intentionally load the host while this run is active.

## 9. Seal the final no-compensation decision

Only after all five raw artifact slots and five receipts exist:

```powershell
python scripts/m8_final_evidence_binder.py --artifact-root $Bundle
$BinderExit = $LASTEXITCODE

Get-Content (Join-Path $Bundle 'final-certification.json')

if ($BinderExit -eq 1) {
    throw "M8 evidence bundle is invalid"
}
if ($BinderExit -eq 2) {
    throw "M8 evidence is valid but at least one certification gate failed"
}
if ($BinderExit -ne 0) {
    throw "Unexpected M8 binder exit: $BinderExit"
}
```

Exit `0` is the only final M8 PASS. `final-certification.json` is create-only and may not be replaced.

## 10. Publication checklist

A final M8 publication must preserve together:

- `bundle-plan.json`;
- all five raw artifacts;
- all five receipts;
- `final-certification.json`;
- exact candidate commit/tree;
- exact executable hashes recorded by runner receipts;
- physical/raw profile collector receipts;
- failure evidence from any independent failed bundle rather than deleting it.

No result from another bundle may replace one axis in this bundle.

## M7 remains independent

A passing M8 bundle does not manufacture M7 competitor leadership evidence. M7 still requires the real physical six-runtime readiness preflight, canonical 6x2 browser collection, both physical Zevryon modes, v2 collection admission, fixed five-metric evaluator and canonical publication manifest on the final physical benchmark host.