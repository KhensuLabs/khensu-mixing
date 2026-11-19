# packaging/build-python-env.ps1
param(
    [Parameter(Mandatory = $true)]
    [string]$OutputDir,

    [string]$PythonVersion = "3.11.9",

    [string]$ModelsDir = $null,  # defaults to sibling "models" dir next to $OutputDir if unset

    [switch]$IncludeStems
)

$ErrorActionPreference = "Stop"

# --- Pinned SHA-256 digests for base remote artifacts (audit CFG-03) --------
# The optional Demucs profile has its own reviewed lock and installer:
# packaging/install-optional-stack.ps1 reads local-services/demucs-stems-lock.json.
# A mismatch means the host served different bytes than this script was written
# against - fail closed and investigate; NEVER weaken this to a size check.
#
# Changing $PythonVersion requires supplying the matching digest - there is
# deliberately no unpinned fallback.
$PinnedSha256 = @{
    "python-3.11.9-embed-amd64.zip" = "009D6BF7E3B2DDCA3D784FA09F90FE54336D5B60F0E0F305C37F400BF83CFD3B"
    "get-pip.py"                    = "FB24E693BAB954209A063D90953621412CCAD4A500905A726286E038F508DDF6"
}

function Get-VerifiedDownload {
    param([string]$Url, [string]$OutFile, [string]$PinKey)

    $expected = $PinnedSha256[$PinKey]
    if (-not $expected) { throw "No pinned digest for '$PinKey' - refusing to download unverified." }

    Write-Host "Downloading $Url ..."
    Invoke-WebRequest -Uri $Url -OutFile $OutFile
    $actual = (Get-FileHash -LiteralPath $OutFile -Algorithm SHA256).Hash
    if ($actual -ne $expected) {
        Remove-Item -LiteralPath $OutFile -Force -ErrorAction SilentlyContinue
        throw "SHA-256 mismatch for $PinKey`n  expected $expected`n  actual   $actual`nThe host served different bytes than this script was pinned against - investigate before proceeding."
    }
    Write-Host "Verified SHA-256 for $PinKey."
}

function Get-EmbeddablePython {
    param([string]$Version, [string]$Dest)

    if (Test-Path "$Dest\python.exe") {
        Write-Host "Embeddable Python already present at $Dest, skipping download."
        return
    }

    New-Item -ItemType Directory -Force -Path $Dest | Out-Null
    $zipName = "python-$Version-embed-amd64.zip"
    $zipUrl = "https://www.python.org/ftp/python/$Version/$zipName"
    $zipPath = Join-Path $env:TEMP $zipName

    Get-VerifiedDownload -Url $zipUrl -OutFile $zipPath -PinKey $zipName

    Write-Host "Extracting to $Dest ..."
    Expand-Archive -Path $zipPath -DestinationPath $Dest -Force
    Remove-Item $zipPath
}

function Enable-SitePackages {
    param([string]$Dest, [string]$Version)

    # Embeddable Python ships with `import site` commented out and no
    # site-packages path in its ._pth file — pip and third-party packages
    # won't be importable until we fix both.
    $shortVer = ($Version -split '\.')[0..1] -join ''
    $pthFile = Join-Path $Dest "python$shortVer._pth"

    if (-not (Test-Path $pthFile)) {
        throw "Expected ._pth file not found at $pthFile — embeddable package layout may have changed."
    }

    $content = Get-Content $pthFile
    $content = $content -replace '^#import site$', 'import site'
    if ($content -notcontains 'Lib\site-packages') {
        $content += 'Lib\site-packages'
    }
    Set-Content -Path $pthFile -Value $content

    Write-Host "Enabled site-packages imports in $pthFile"
}

function Install-Pip {
    param([string]$Dest)

    $pyExe = Join-Path $Dest "python.exe"
    $probe = & $pyExe -m pip --version 2>&1
    if ($LASTEXITCODE -eq 0) {
        Write-Host "pip already available in $Dest, skipping bootstrap."
        return
    }

    $getPipPath = Join-Path $env:TEMP "get-pip.py"
    # A pinned-commit raw URL, not bootstrap.pypa.io: the bootstrap URL's
    # content rotates with pip releases, which would break the digest pin on
    # pypa's schedule instead of ours. This commit is immutable.
    $getPipUrl = "https://raw.githubusercontent.com/pypa/get-pip/f6f644156f23dfe9acc06e7b9ca75eee311f2e37/public/get-pip.py"
    Get-VerifiedDownload -Url $getPipUrl -OutFile $getPipPath -PinKey "get-pip.py"

    Write-Host "Bootstrapping pip ..."
    & $pyExe $getPipPath --no-warn-script-location
    if ($LASTEXITCODE -ne 0) {
        throw "pip bootstrap failed with exit code $LASTEXITCODE"
    }
    Remove-Item $getPipPath
}

function Install-LocalServicesBase {
    param([string]$Dest, [string]$LocalServicesDir)

    $pyExe = Join-Path $Dest "python.exe"
    $hashedRequirements = Join-Path $LocalServicesDir "requirements-runtime-hashed.txt"
    if (-not (Test-Path $hashedRequirements)) {
        throw "Hash-locked requirements artifact is missing: $hashedRequirements"
    }

    Write-Host "Installing hash-verified local-services dependencies ..."
    & $pyExe -m pip install --no-cache-dir --require-hashes -r $hashedRequirements
    if ($LASTEXITCODE -ne 0) { throw "hash-verified dependency install failed with exit code $LASTEXITCODE" }

    # Hash mode intentionally forbids editable/local project installation.
    # Dependencies are fully pinned above; install the reviewed local package
    # without resolving its floating pyproject ranges again.
    Write-Host "Installing reviewed local-services package without dependency resolution ..."
    & $pyExe -m pip install --no-cache-dir --no-deps "$LocalServicesDir"
    if ($LASTEXITCODE -ne 0) { throw "local-services package install failed with exit code $LASTEXITCODE" }

    $probe = & $pyExe -c "import fastapi, uvicorn, librosa, soundfile, khensu_services" 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "Post-install sanity check failed: base packages / khensu_services not importable.`n$probe"
    }
    Write-Host "Verified fastapi/uvicorn/librosa/soundfile/khensu_services import OK."
}

$LocalServicesDir = Join-Path (Split-Path $PSScriptRoot -Parent) "local-services"

Get-EmbeddablePython -Version $PythonVersion -Dest $OutputDir
Enable-SitePackages -Dest $OutputDir -Version $PythonVersion
Install-Pip -Dest $OutputDir
Install-LocalServicesBase -Dest $OutputDir -LocalServicesDir $LocalServicesDir

# MMC Locate's port backend (the [midi] extra: python-rtmidi). Optional: a
# failure leaves MMC off rather than failing the runtime build.
$midiRequirements = Join-Path $LocalServicesDir "requirements-midi-hashed.txt"
if (Test-Path $midiRequirements) {
    & (Join-Path $OutputDir "python.exe") -m pip install --no-cache-dir --require-hashes -r $midiRequirements
    if ($LASTEXITCODE -ne 0) { Write-Warning "python-rtmidi did not install; MMC Locate will be off." }
    else { Write-Host "Installed python-rtmidi (MMC Locate)." }
}
$pyExe = Join-Path $OutputDir "python.exe"

# The stems profile is opt-in because it adds a large CPU ML runtime. Its
# installer owns the full reviewed lock, hash manifest, pinned Demucs source,
# and post-install import probe; do not replace this with floating pip extras.
if ($IncludeStems) {
    & (Join-Path $PSScriptRoot "install-optional-stack.ps1") -Stack stems -PythonExe $pyExe
    if ($LASTEXITCODE -ne 0) { throw "Reviewed stems profile installation failed with exit code $LASTEXITCODE" }
}

& $pyExe (Join-Path $LocalServicesDir "launch_service.py") --smoke
if ($LASTEXITCODE -ne 0) { throw "Staged runtime smoke check failed with exit code $LASTEXITCODE" }

Write-Host "Build complete. Hash-verified runtime: $OutputDir"
