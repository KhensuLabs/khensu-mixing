<#
.SYNOPSIS
  Checks packaging/sign-artifacts.ps1 (SHIP-3): the preview mode still lists
  and exits 0, and -Require - the mode a release uses - fails when there is no
  certificate or a shipped binary is missing, instead of passing unsigned.
  No certificate and no signtool are needed. Exits non-zero on a failure.

  pwsh scripts/tests/test_sign_artifacts.ps1
#>
$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$script = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) "packaging\sign-artifacts.ps1"
$failures = [System.Collections.Generic.List[string]]::new()
function Check([string]$name, [bool]$ok, [string]$detail = "") {
    if ($ok) { Write-Host "  ok    $name" }
    else { Write-Host "  FAIL  $name $detail" -ForegroundColor Red; $failures.Add($name) }
}
function Run-Sign([string[]]$arguments) {
    $out = & pwsh -NoProfile -File $script @arguments 2>&1 | ForEach-Object { "$_" }
    [pscustomobject]@{ Code = $LASTEXITCODE; Text = ($out -join "`n") }
}

$root = Join-Path ([System.IO.Path]::GetTempPath()) ("khensu-sign-test-" + [guid]::NewGuid().ToString("N"))
$full = Join-Path $root "full"
$empty = Join-Path $root "empty"
$appRel = "desktop-app\KhensuDesktop_artefacts\Release\Khensu Mixing.exe"
$vstRel = "plugin-copilot\KhensuCopilot_artefacts\Release\VST3\Khensu Copilot.vst3\Contents\x86_64-win\Khensu Copilot.vst3"
try {
    foreach ($rel in @($appRel, $vstRel)) {
        $p = Join-Path $full $rel
        [void](New-Item -ItemType Directory -Force -Path (Split-Path -Parent $p))
        Set-Content -LiteralPath $p -Value "not a real binary"
    }
    [void](New-Item -ItemType Directory -Force -Path $empty)

    # 1. Preview: no certificate, no -Require. Lists the targets, exits 0.
    $r = Run-Sign @("-BuildDir", $full)
    Check "preview exits 0" ($r.Code -eq 0) "got $($r.Code): $($r.Text)"
    Check "preview lists the app" ($r.Text -match "Khensu Mixing\.exe") $r.Text
    Check "preview says nothing was signed" ($r.Text -match "(?i)nothing was signed") $r.Text

    # 2. Release mode with no certificate must fail.
    $r = Run-Sign @("-BuildDir", $full, "-Require")
    Check "-Require without a certificate fails" ($r.Code -ne 0) "got $($r.Code): $($r.Text)"
    Check "-Require without a certificate says why" ($r.Text -match "(?i)must be signed: no certificate") $r.Text

    # 3. Release mode with a certificate but no built app must fail before
    #    signing anything, and name what is missing.
    $r = Run-Sign @("-BuildDir", $empty, "-Require", "-Thumbprint", "0000000000000000000000000000000000000000")
    Check "-Require with a missing binary fails" ($r.Code -ne 0) "got $($r.Code): $($r.Text)"
    Check "-Require names the missing binary" ($r.Text -match "Khensu Mixing\.exe") $r.Text

    # 4. A named installer that does not exist is an error in release mode.
    $r = Run-Sign @("-BuildDir", $full, "-Require", "-Thumbprint", "0000000000000000000000000000000000000000",
                    "-Installer", (Join-Path $root "no-such-setup.exe"))
    Check "-Require with a missing installer fails" ($r.Code -ne 0) "got $($r.Code)"
    Check "-Require names the missing installer" ($r.Text -match "no-such-setup\.exe") $r.Text
}
finally {
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}

if ($failures.Count) { Write-Host "test_sign_artifacts: FAILED ($($failures.Count))" -ForegroundColor Red; exit 1 }
Write-Host "test_sign_artifacts: OK" -ForegroundColor Green
exit 0
