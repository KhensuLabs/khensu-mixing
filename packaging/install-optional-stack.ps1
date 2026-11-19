# packaging/install-optional-stack.ps1
# Installs reviewed optional stacks into existing environments. The caller owns
# environment creation and base-runtime installation; this script only adds a
# hash-verified profile and fails closed on every remote Demucs artifact.
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("stems", "ml")]
    [string]$Stack,

    [Parameter(Mandatory = $true)]
    [string]$PythonExe,

    [string]$PitchPythonExe = $null
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path $PSScriptRoot -Parent
$LocalServicesDir = Join-Path $RepoRoot "local-services"
$LockPath = Join-Path $LocalServicesDir "demucs-stems-lock.json"

if (-not (Test-Path -LiteralPath $PythonExe)) {
    throw "Python executable not found: $PythonExe"
}
if (-not (Test-Path -LiteralPath $LockPath)) {
    throw "Reviewed Demucs lock is missing: $LockPath"
}

$DemucsLock = Get-Content -LiteralPath $LockPath -Raw | ConvertFrom-Json
if ($DemucsLock.target.platform -ne "win_amd64" -or $DemucsLock.target.python -ne "3.11") {
    throw "Demucs lock target is not the reviewed Windows x64 Python 3.11 profile."
}

function Assert-ExitCode {
    param([string]$Action)
    if ($LASTEXITCODE -ne 0) {
        throw "$Action failed with exit code $LASTEXITCODE"
    }
}

function Get-VerifiedDownload {
    param([string]$Url, [string]$OutFile, [string]$ExpectedSha256, [string]$ArtifactName)

    Write-Host "Downloading $ArtifactName ..."
    Invoke-WebRequest -Uri $Url -OutFile $OutFile
    $actual = (Get-FileHash -LiteralPath $OutFile -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $ExpectedSha256.ToLowerInvariant()) {
        Remove-Item -LiteralPath $OutFile -Force -ErrorAction SilentlyContinue
        throw "SHA-256 mismatch for $ArtifactName`n  expected $ExpectedSha256`n  actual   $actual`nRefusing unreviewed bytes."
    }
    Write-Host "Verified SHA-256 for $ArtifactName."
}

function Install-Stems {
    & $PythonExe -c "import platform, sys; raise SystemExit(0 if sys.version_info[:2] == (3, 11) and platform.machine().lower() in ('amd64', 'x86_64') else 1)"
    Assert-ExitCode "Stems environment target check"

    $manifest = Join-Path $LocalServicesDir $DemucsLock.requirements_manifest
    if (-not (Test-Path -LiteralPath $manifest)) {
        throw "Reviewed Demucs requirements manifest is missing: $manifest"
    }

    Write-Host "Installing hash-verified Demucs dependencies ..."
    & $PythonExe -m pip install --no-cache-dir --require-hashes -r $manifest
    Assert-ExitCode "Demucs dependency install"

    $archive = Join-Path ([System.IO.Path]::GetTempPath()) ("demucs-" + $DemucsLock.demucs.commit + ".zip")
    try {
        Get-VerifiedDownload -Url $DemucsLock.demucs.archive_url -OutFile $archive `
            -ExpectedSha256 $DemucsLock.demucs.sha256 -ArtifactName "Demucs source archive $($DemucsLock.demucs.commit)"
        & $PythonExe -m pip install --no-cache-dir --no-deps $archive
        Assert-ExitCode "Pinned Demucs source install"
    }
    finally {
        Remove-Item -LiteralPath $archive -Force -ErrorAction SilentlyContinue
    }

    # The versions come from the reviewed lock, so a lock bump can't leave
    # this probe asserting the previous pair (PKG-1 moved 2.8.0 -> 2.11).
    $pin = @{}
    foreach ($entry in $DemucsLock.packages) {
        $parts = $entry -split '==', 2
        $pin[$parts[0]] = $parts[1]
    }
    $torchPin = $pin['torch']
    $audioPin = $pin['torchaudio']
    $probe = & $PythonExe -c "import torch, torchaudio; from demucs.api import Separator; assert torch.__version__.split('+')[0] == '$torchPin'; assert torchaudio.__version__.split('+')[0] == '$audioPin'" 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "Post-install stems probe failed: torch $torchPin / torchaudio $audioPin and demucs.api.Separator are required.`n$probe"
    }
    Write-Host "Verified reviewed Demucs stems profile."
}

function Install-Pitch {
    if (-not $PitchPythonExe) {
        $PitchPythonExe = Join-Path $LocalServicesDir ".venv311\Scripts\python.exe"
    }
    if (-not (Test-Path -LiteralPath $PitchPythonExe)) {
        throw "The ml profile needs the dedicated Python 3.11 pitch environment. Create local-services/.venv311 or pass -PitchPythonExe."
    }
    & $PitchPythonExe -c "import sys; raise SystemExit(0 if sys.version_info[:2] == (3, 11) else 1)"
    Assert-ExitCode "Pitch environment version check"

    $manifest = Join-Path $LocalServicesDir "requirements-pitch-hashed.txt"
    Write-Host "Installing hash-verified pitch dependencies in the dedicated Python 3.11 environment ..."
    # --no-deps: the manifest is the complete, hash-verified closure, and
    # basic-pitch's metadata would otherwise pull TensorFlow, which the
    # profile deliberately leaves out (it runs basic-pitch's ONNX model).
    & $PitchPythonExe -m pip install --no-cache-dir --no-deps --require-hashes -r $manifest
    Assert-ExitCode "Pitch dependency install"
    & $PitchPythonExe -c "import onnxruntime, basic_pitch; from basic_pitch import ICASSP_2022_MODEL_PATH; import sys; sys.exit(0 if str(ICASSP_2022_MODEL_PATH).endswith('.onnx') else 1)"
    Assert-ExitCode "Pitch profile probe (basic-pitch on its ONNX model)"
    & $PitchPythonExe -m pip install --no-cache-dir --no-deps $LocalServicesDir
    Assert-ExitCode "Local services package install in pitch environment"
}

Install-Stems
if ($Stack -eq "ml") {
    Install-Pitch
}
