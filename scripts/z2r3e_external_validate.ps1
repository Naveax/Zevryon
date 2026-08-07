[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9a-fA-F]{40}$')]
    [string]$ExpectedHead,

    [string]$Compiler = 'msvc-release',

    [string]$RepositoryUrl = 'https://github.com/Naveax/Zevryon.git',

    [string]$OutputDirectory = $(
        if (Test-Path (Join-Path $HOME 'Downloads')) {
            Join-Path $HOME 'Downloads'
        } else {
            (Get-Location).Path
        }
    )
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Invoke-Native {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FilePath,

        [Parameter(ValueFromRemainingArguments = $true)]
        [string[]]$Arguments
    )

    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Native command failed ($LASTEXITCODE): $FilePath $($Arguments -join ' ')"
    }
}

foreach ($tool in @('git', 'python', 'cmake', 'ctest', 'cargo', 'rustc')) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "Required tool is unavailable: $tool"
    }
}

$shortHead = $ExpectedHead.Substring(0, 12).ToLowerInvariant()
$stamp = (Get-Date).ToUniversalTime().ToString('yyyyMMddTHHmmssZ')
$workRoot = Join-Path ([System.IO.Path]::GetTempPath()) "zevryon-z2r3eu-$shortHead-$stamp"
$cloneRoot = Join-Path $workRoot 'repo'
$evidenceRoot = Join-Path $cloneRoot 'evidence\z2r3eu'
$summaryPath = Join-Path $evidenceRoot 'windows-authority-validation.json'
$packageBase = "zevryon-z2r3eu-windows-$shortHead-$stamp"
$archivePath = Join-Path $OutputDirectory "$packageBase.zip"
$hashPath = "$archivePath.sha256"
$validationExit = 1

New-Item -ItemType Directory -Force -Path $workRoot, $OutputDirectory | Out-Null

try {
    Invoke-Native git clone --no-tags --filter=blob:none --single-branch --branch agent/z2r3eu-unicode-authority $RepositoryUrl $cloneRoot
    Push-Location $cloneRoot
    try {
        Invoke-Native git fetch --no-tags origin $ExpectedHead
        Invoke-Native git checkout --detach $ExpectedHead

        $actualHead = (& git rev-parse HEAD).Trim().ToLowerInvariant()
        if ($LASTEXITCODE -ne 0 -or $actualHead -ne $ExpectedHead.ToLowerInvariant()) {
            throw "Exact-head mismatch. Expected $ExpectedHead, got $actualHead"
        }

        $dirty = & git status --porcelain=v1
        if ($LASTEXITCODE -ne 0 -or $dirty) {
            throw 'Validation checkout is not clean.'
        }

        New-Item -ItemType Directory -Force -Path $evidenceRoot | Out-Null
        @(
            "expected_head=$ExpectedHead"
            "actual_head=$actualHead"
            "repository=$RepositoryUrl"
            "platform=windows"
            "compiler=$Compiler"
            "started_utc=$((Get-Date).ToUniversalTime().ToString('o'))"
        ) | Set-Content -Encoding UTF8 (Join-Path $evidenceRoot 'external-run.txt')

        & python scripts/z2r3e_validate_unicode_authority.py `
            --sha $ExpectedHead `
            --platform windows `
            --compiler $Compiler `
            --output $summaryPath
        $validationExit = $LASTEXITCODE
    } finally {
        Pop-Location
    }
} finally {
    if (Test-Path $evidenceRoot) {
        if (Test-Path $archivePath) {
            Remove-Item -Force $archivePath
        }
        Compress-Archive -Path (Join-Path $evidenceRoot '*') -DestinationPath $archivePath -CompressionLevel Optimal
        $hash = (Get-FileHash -Algorithm SHA256 $archivePath).Hash.ToLowerInvariant()
        "$hash  $([System.IO.Path]::GetFileName($archivePath))" | Set-Content -Encoding ASCII $hashPath
        Write-Host "Evidence ZIP: $archivePath" -ForegroundColor Cyan
        Write-Host "SHA-256:     $hashPath" -ForegroundColor Cyan
    }
}

if ($validationExit -ne 0) {
    throw "Z2R-3E-U Windows validation failed with exit code $validationExit. Evidence was preserved."
}

Write-Host "Z2R-3E-U Windows exact-head validation passed: $ExpectedHead" -ForegroundColor Green
