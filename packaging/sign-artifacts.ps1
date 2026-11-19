<#
.SYNOPSIS
  Authenticode-signs the built desktop app, plugin binaries and (optionally)
  the installer with signtool (CE-ENG-2).

.DESCRIPTION
  The certificate comes from either a PFX file (-PfxPath plus the
  KHENSU_SIGN_PFX_PASSWORD environment variable, so the password never lands
  in shell history) or a certificate already in the Windows store
  (-Thumbprint). Every file is timestamped (RFC 3161) and then verified with
  `signtool verify /pa`.

  Two modes. Without -Require this is a preview: with no certificate it lists
  what it would sign and exits 0. A release passes -Require (SHIP-3): then no
  certificate, a missing app or VST3 binary, or a named installer that does not
  exist is an error, so an unsigned release cannot pass as a signed one.

.EXAMPLE
  $env:KHENSU_SIGN_PFX_PASSWORD = '...'
  pwsh packaging/sign-artifacts.ps1 -Require -PfxPath C:\certs\khensu.pfx
.EXAMPLE
  pwsh packaging/sign-artifacts.ps1 -Thumbprint 0123ABCD... -Installer .build-artifacts\installer\khensu-setup-1.0.0.exe
#>
param(
    [string]$PfxPath,
    [string]$Thumbprint,
    [string]$TimestampUrl = "http://timestamp.digicert.com",
    [string]$Installer,
    [string]$BuildDir = (Join-Path $PSScriptRoot "..\build"),
    # Release mode: fail instead of passing with nothing signed.
    [switch]$Require
)
$ErrorActionPreference = "Stop"

function Find-SignTool {
    $cmd = Get-Command signtool.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $kits = "${env:ProgramFiles(x86)}\Windows Kits\10\bin"
    if (Test-Path $kits) {
        $found = Get-ChildItem $kits -Recurse -Filter signtool.exe -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match "\\x64\\" } | Sort-Object FullName -Descending | Select-Object -First 1
        if ($found) { return $found.FullName }
    }
    throw "signtool.exe not found (install the Windows 10/11 SDK)."
}

# What the installer always ships. The CLAP and ARA builds are signed when
# they exist; they are not in the installer yet (SHIP-1).
$required = @(
    (Join-Path $BuildDir "desktop-app\KhensuDesktop_artefacts\Release\Khensu Mixing.exe"),
    (Join-Path $BuildDir "plugin-copilot\KhensuCopilot_artefacts\Release\VST3\Khensu Copilot.vst3\Contents\x86_64-win\Khensu Copilot.vst3")
)
$optional = @(
    (Join-Path $BuildDir "plugin-copilot\KhensuCopilot_artefacts\Release\CLAP\Khensu Copilot.clap"),
    (Join-Path $BuildDir "plugin-copilot\KhensuCopilotARA_artefacts\Release\VST3\Khensu Copilot ARA.vst3\Contents\x86_64-win\Khensu Copilot ARA.vst3")
)
if ($Installer) { $required += $Installer }
$missing = @($required | Where-Object { -not (Test-Path -LiteralPath $_) })
$targets = @(($required + $optional) | Where-Object { Test-Path -LiteralPath $_ })

if ($Require) {
    # Checked before signtool runs, so a half-signed set is never left behind.
    if ($missing.Count) {
        Write-Host "A release must be signed, and these are missing:" -ForegroundColor Red
        $missing | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
        exit 2
    }
    if (-not $PfxPath -and -not $Thumbprint) {
        Write-Host "A release must be signed: no certificate given (-PfxPath or -Thumbprint)." -ForegroundColor Red
        Write-Host "Nothing was signed. Unsigned:" -ForegroundColor Red
        $targets | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
        exit 2
    }
}
elseif (-not $PfxPath -and -not $Thumbprint) {
    Write-Host "Preview: no certificate given (-PfxPath or -Thumbprint), nothing was signed. Would sign:"
    $targets | ForEach-Object { Write-Host "  $_" }
    Write-Host "A release must pass -Require, which fails here instead."
    exit 0
}

$signtool = Find-SignTool
$certArgs = @()
if ($PfxPath) {
    if (-not $env:KHENSU_SIGN_PFX_PASSWORD) { throw "Set KHENSU_SIGN_PFX_PASSWORD for the PFX." }
    $certArgs = @("/f", $PfxPath, "/p", $env:KHENSU_SIGN_PFX_PASSWORD)
} else {
    $certArgs = @("/sha1", $Thumbprint)
}

foreach ($t in $targets) {
    Write-Host "signing $t"
    & $signtool sign /fd sha256 /tr $TimestampUrl /td sha256 @certArgs $t
    if ($LASTEXITCODE -ne 0) { throw "signtool sign failed for $t" }
    & $signtool verify /pa $t
    if ($LASTEXITCODE -ne 0) { throw "signtool verify failed for $t" }
}
Write-Host "signed $($targets.Count) file(s)"
