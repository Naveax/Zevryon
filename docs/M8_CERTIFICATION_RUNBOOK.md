# M8 canonical certification runbook v2

## Status

This runbook is the canonical execution order for one final M8 no-compensation evidence bundle after binder v2 is admitted on `main`.

Do not execute a production PASS claim from the binder-v2 candidate merely because its files exist on a branch. The binder/importer stack must first receive its exact-head natural CI admission and be merged to canonical `main`.

## Non-negotiable rules

- One final decision uses one exact clean Git commit/tree and one frozen bundle.
- Never edit tracked source after freezing the bundle.
- Build output and long-run evidence live outside the repository.
- Every artifact/receipt slot is create-only.
- Never delete a failed slot and rerun it in the same bundle.
- Never cherry-pick successful axes from different bundles.
- A short CI smoke is not certification evidence.
- Fresh-process crash evidence is not physical power-loss evidence.
- Four profile observations must come from four physical collector attempts, not a prepared observations JSON.
- Preserve failed bundles and failed profile attempts.
- During the 24-hour soak, keep the host dedicated and do not suspend/hibernate it.

## 1. Pin one clean candidate

Run from the repository root after all required M8 authorities are admitted to canonical `main`:

```powershell
git status --porcelain=v1 --untracked-files=all
$Commit = (git rev-parse HEAD).Trim()
$Tree = (git rev-parse 'HEAD^{tree}').Trim()
$Commit
$Tree
```

`git status` must print nothing.

Record the exact commit/tree. Every Titan report, physical attempt, bundle plan and long-run receipt must bind this candidate.

## 2. Create a fresh external Release build

The coordinator example uses Ninja so target paths do not depend on a Visual Studio configuration subdirectory.

```powershell
$Repo = (Get-Location).Path
$Build = "C:\Zevryon-M8-Build\$Commit"

if (Test-Path -LiteralPath $Build) {
    throw "Refusing to reuse certification build directory: $Build"
}

cmake -S $Repo -B $Build -G Ninja `
    -DCMAKE_BUILD_TYPE=Release `
    -DBUILD_TESTING=ON

cmake --build $Build --target `
    zevryon-m8-storage-crash-probe `
    zevryon-m8-mixed-mutation-probe `
    zevryon-m8-continuous-soak-probe `
    zevryon-m8-property-fuzz-probe `
    zevryon-m8-profile-case-probe

ctest --test-dir $Build --output-on-failure -R '^m8-'
if ($LASTEXITCODE -ne 0) {
    throw "M8 pre-certification smoke suite failed"
}

if (git status --porcelain=v1 --untracked-files=all) {
    throw "Repository became dirty during external build/smoke"
}
```

Bind coordinator binaries on Windows:

```powershell
$StorageProbe = Join-Path $Build 'zevryon-m8-storage-crash-probe.exe'
$MixedProbe   = Join-Path $Build 'zevryon-m8-mixed-mutation-probe.exe'
$SoakProbe    = Join-Path $Build 'zevryon-m8-continuous-soak-probe.exe'
$FuzzProbe    = Join-Path $Build 'zevryon-m8-property-fuzz-probe.exe'
```

On Linux use the same target names without `.exe`.

## 3. Generate the canonical certification Titan

Use a fresh external directory and do not overwrite an earlier Titan attempt.

```powershell
$TitanRoot = "C:\Zevryon-M8-Titan\$Commit"
$Titan = Join-Path $TitanRoot 'titan.zmdoc'
$TitanReport = Join-Path $TitanRoot 'titan-report.json'

if (Test-Path -LiteralPath $TitanRoot) {
    throw "Titan root already exists: $TitanRoot"
}
New-Item -ItemType Directory -Path $TitanRoot | Out-Null

python scripts/m8_titan_fixture.py `
    --certification `
    --output $Titan `
    --report $TitanReport

if ($LASTEXITCODE -ne 0) {
    throw "Canonical Titan generation/verification failed"
}
```

Do not override the certification envelope with smaller custom values. Certification mode must satisfy the full frozen envelope, including the 4 GiB logical UTF-8 payload and the giant-record, unbroken-token and pathological-grapheme dimensions.

Record hashes:

```powershell
Get-FileHash -Algorithm SHA256 $Titan
Get-FileHash -Algorithm SHA256 $TitanReport
```

Preserve the Titan container/report for the lifetime of the certification archive.

## 4. Prepare one exact Linux profile-probe binary

The current physical profile collector obtains aggregate process-group PSS from Linux `/proc/<pid>/smaps_rollup`. Therefore the four profile attempts must be collected on Linux-compatible physical hosts that satisfy the M0 physical-host authority.

Build `zevryon-m8-profile-case-probe` once for the OS/architecture shared by those hosts and distribute that **exact binary** unchanged to all four hosts. Do not independently rebuild four binaries and assume they are byte-identical.

Record its SHA-256 before distribution:

```bash
sha256sum zevryon-m8-profile-case-probe
```

The importer later requires one shared `--probe` file and independently checks every attempt against that same probe SHA.

Each physical host must also have a clean checkout of the exact candidate commit/tree so the collector scripts bind the same source identity.

## 5. Collect exactly four physical profile attempts

Required profiles:

- `legacy-phone`;
- `mid-phone`;
- `modern-phone`;
- `desktop`.

The requested profile must equal the real M0 physical-host classification. Identity/profile overrides are rejected.

On each qualified Linux host, from a clean checkout of the exact candidate, use the same Titan/report and the exact shared profile-probe binary.

Example for one host:

```bash
python3 scripts/m8_profile_case_collector.py \
  --probe /opt/zevryon-m8/zevryon-m8-profile-case-probe \
  --profile legacy-phone \
  --titan /evidence/titan.zmdoc \
  --titan-report /evidence/titan-report.json \
  --attempt-dir /evidence/profile-attempts/legacy-phone
```

Repeat once for `mid-phone`, `modern-phone` and `desktop`, each on a host that independently qualifies for that class.

Every attempt directory is create-only and must preserve at least:

- `profile-case.json`;
- `profile-case-provenance.json`;
- `physical-host.json`;
- `probe-work/copy-output.txt`.

The copy output is large and is part of the external provenance chain. Preserve every original attempt directory even though the final bundle imports compact support receipts rather than duplicating every copy-output byte.

If a collector attempt fails, preserve it and use a new attempt directory for any independent repeat. Do not edit the failed attempt into a passing one.

## 6. Freeze one fresh final bundle

Return to the coordinator checkout at the exact candidate commit/tree.

```powershell
if (git status --porcelain=v1 --untracked-files=all) {
    throw "Candidate checkout is dirty"
}
if ((git rev-parse HEAD).Trim() -ne $Commit) {
    throw "Candidate commit drifted"
}
if ((git rev-parse 'HEAD^{tree}').Trim() -ne $Tree) {
    throw "Candidate tree drifted"
}

$Bundle = "C:\Zevryon-M8-Evidence\$Commit"
if (Test-Path -LiteralPath $Bundle) {
    throw "Bundle path already exists: $Bundle"
}

python scripts/m8_freeze_bundle.py --artifact-root $Bundle
if ($LASTEXITCODE -ne 0) {
    throw "M8 bundle freeze failed"
}
```

`bundle-plan.json` now seals the bundle id, candidate commit/tree, fixed artifact/receipt paths and frozen authority-source hashes.

## 7. Import the four physical attempts through binder v2

The coordinator must have the exact Titan/report and the exact shared Linux profile-probe binary used by the four physical collectors. The probe file is hashed as evidence; it does not need to execute on the coordinator during import.

Example:

```powershell
$ProfileProbeEvidence = 'C:\Zevryon-M8-Profile-Evidence\zevryon-m8-profile-case-probe'
$LegacyAttempt = 'C:\Zevryon-M8-Profile-Evidence\attempts\legacy-phone'
$MidAttempt    = 'C:\Zevryon-M8-Profile-Evidence\attempts\mid-phone'
$ModernAttempt = 'C:\Zevryon-M8-Profile-Evidence\attempts\modern-phone'
$DesktopAttempt= 'C:\Zevryon-M8-Profile-Evidence\attempts\desktop'

python scripts/m8_bundle_import_profile.py `
    --artifact-root $Bundle `
    --titan-report $TitanReport `
    --titan $Titan `
    --probe $ProfileProbeEvidence `
    --attempt "legacy-phone=$LegacyAttempt" `
    --attempt "mid-phone=$MidAttempt" `
    --attempt "modern-phone=$ModernAttempt" `
    --attempt "desktop=$DesktopAttempt"

$ProfileExit = $LASTEXITCODE
if ($ProfileExit -eq 1) {
    throw "Profile evidence/import is invalid; preserve the bundle"
}
if ($ProfileExit -eq 2) {
    throw "Profile evidence is valid but one or more no-compensation profile gates failed; preserve the bundle"
}
if ($ProfileExit -ne 0) {
    throw "Unexpected profile importer exit: $ProfileExit"
}
```

The importer reruns the independent provenance verifier for all four attempts, creates `profile-observations.json`, and seals immutable `profile-support/` evidence plus the profile receipt.

Do not replace this step with a hand-authored observations file.

## 8. Collect the full fresh-process storage crash matrix

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
    throw "Storage crash slot failed; preserve it and do not rerun this slot"
}
```

This executes all five publication cuts and all three compaction cuts with abrupt child-process exit code `86` and fresh recovery processes.

It does **not** certify physical power loss.

## 9. Execute at least 10,000,000 mixed mutations

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
    throw "Mixed-mutation slot failed; preserve it and do not rerun this slot"
}
```

The final binder independently requires all five mutation classes, exact completed-count accounting, verification checkpoints, equal nonzero live/oracle order digests and zero integrity/order mismatches.

## 10. Execute at least 10,000 cases in each fuzz domain

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
    throw "Property-fuzz slot failed; preserve it and do not rerun this slot"
}
```

The exact required domains are Unicode, serializer, index and sequence. Every domain must complete the full requested count with zero failures.

## 11. Execute one continuous >=86,400-second soak

Run this only when the host can remain dedicated for the full measured interval.

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
    throw "Continuous-soak slot failed; preserve it and do not rerun this slot"
}
```

Certification mode freezes the 60-second checkpoint interval and one-second memory sampling interval. The 86,400-second certification timer is measured after setup/warmup.

Do not suspend or hibernate the host, pause the process, rebuild the candidate, modify tracked source, or deliberately load the machine with unrelated benchmarks/games/stress jobs during the soak.

## 12. Seal the final no-compensation decision exactly once

Only after all five artifact slots and receipts exist:

```powershell
python scripts/m8_final_evidence_binder.py --artifact-root $Bundle
$BinderExit = $LASTEXITCODE

Get-Content (Join-Path $Bundle 'final-certification.json')

if ($BinderExit -eq 1) {
    throw "M8 evidence package is invalid"
}
if ($BinderExit -eq 2) {
    throw "M8 evidence is valid but at least one certification gate failed"
}
if ($BinderExit -ne 0) {
    throw "Unexpected M8 binder exit: $BinderExit"
}
```

Exit `0` is the only M8 PASS.

`final-certification.json` is create-only. Do not rerun the binder on the same bundle to replace a decision.

## 13. Archive the complete evidence set

Preserve together:

- `bundle-plan.json`;
- `profile-observations.json`;
- complete `profile-support/` tree;
- `storage-process-crash.json`;
- `mixed-mutation.json`;
- `continuous-soak.jsonl`;
- `property-fuzz.json`;
- all five bundle receipts;
- `final-certification.json`;
- exact candidate commit/tree;
- canonical Titan container/report;
- the exact shared physical profile-probe binary;
- all four original physical attempt directories, including copy outputs;
- any failed independent attempts/bundles rather than deleting them.

No artifact from another bundle may replace one axis in the final bundle.

## M7 remains independent

A passing M8 bundle does not manufacture competitor-leadership evidence. M7 still requires its own real six-runtime physical preflight, canonical 6x2 browser collection, physical Zevryon cases, v2 admission and fixed five-metric evaluation/publication chain.