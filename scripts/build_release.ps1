<#
.SYNOPSIS
  Builds the Windows release: app and plug-ins, signing when a certificate is
  configured, the installer, SHA256SUMS.txt and the release notes folder
  (OVH-4). It is what 1.0.2 was released with by hand, in one command.

.DESCRIPTION
  1. Deletes the generated *_resources.rc files and reconfigures, so the
     Windows version resources carry this version (JUCE writes them once and
     never rewrites them after a version bump: the 1.0.2 build first said
     1.0.1, and the plug-in 0.1.0).
  2. Builds KhensuDesktop and the plug-ins (Release, --parallel 2) under the
     shared build lock.
  3. Checks both binaries carry the version in Version.h.
  4. Stages the service (scripts/stage_local_services.py --check) and scans
     the stage for secrets.
  5. Signs the app and plug-ins with packaging/sign-artifacts.ps1 -Require
     when a certificate is configured, and compiles the installer with Inno
     Setup's SignTool so the installer and its uninstaller are signed too.
     Without a certificate the installer is built unsigned and the script
     says so; -RequireSigned makes that an error.
  6. Writes khensu-setup-<version>.exe and SHA256SUMS.txt into
     .build-artifacts/release-<version>/.

  A certificate is configured by either
    KHENSU_SIGN_PFX           path to a .pfx, with KHENSU_SIGN_PFX_PASSWORD
    KHENSU_SIGN_THUMBPRINT    a certificate in the Windows store
  (Azure Trusted Signing and other cloud HSMs work through the store / a
  signtool dlib; set KHENSU_SIGN_COMMAND to a full Inno "SignTool" command
  line with $f for the file to override everything.)

.EXAMPLE
  pwsh scripts/build_release.ps1
.EXAMPLE
  $env:KHENSU_SIGN_PFX = 'C:\certs\khensu.pfx'; $env:KHENSU_SIGN_PFX_PASSWORD = '...'
  pwsh scripts/build_release.ps1 -RequireSigned
#>
param(
    [string]$BuildDir = "",
    [switch]$RequireSigned,
    [switch]$SkipBuild
)
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
if (-not $BuildDir) { $BuildDir = Join-Path $Root "build" }
$Python = Join-Path $Root "local-services\.venv\Scripts\python.exe"
if (-not (Test-Path -LiteralPath $Python)) { $Python = "python" }

function Invoke-Checked([string]$exe, [string[]]$arguments) {
    & $exe @arguments
    if ($LASTEXITCODE -ne 0) { throw "$exe $($arguments -join ' ') exited $LASTEXITCODE" }
}

$version = [regex]::Match((Get-Content -Raw -LiteralPath (Join-Path $Root "shared-core\include\khensu\common\Version.h")),
    'KHENSU_VERSION_STRING\s+"([^"]+)"').Groups[1].Value
if (-not $version) { throw "KHENSU_VERSION_STRING not found in Version.h" }
Write-Host "==> Khensu Mixing $version"

$exePath = Join-Path $BuildDir "desktop-app\KhensuDesktop_artefacts\Release\Khensu Mixing.exe"
$vst3Dir = Join-Path $BuildDir "plugin-copilot\KhensuCopilot_artefacts\Release\VST3\Khensu Copilot.vst3"

if (-not $SkipBuild) {
    Write-Host "==> Regenerate version resources and build"
    foreach ($rc in @(Get-ChildItem -LiteralPath $BuildDir -Recurse -Filter "*_resources.rc" -File -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match '\\JuceLibraryCode\\' })) {
        Remove-Item -LiteralPath $rc.FullName -Force
    }
    $lock = Join-Path $PSScriptRoot "with_build_lock.ps1"
    Invoke-Checked "pwsh" @("-NoProfile", "-File", $lock, "cmake", "-S", $Root, "-B", $BuildDir)
    Invoke-Checked "pwsh" @("-NoProfile", "-File", $lock, "cmake", "--build", $BuildDir, "--config", "Release",
        "--parallel", "2", "--target", "KhensuDesktop", "KhensuCopilot_VST3", "KhensuCopilot_CLAP")
}

Write-Host "==> Version resources"
$exeVersion = (Get-Item -LiteralPath $exePath).VersionInfo.ProductVersion
$vst3Dll = Get-ChildItem -LiteralPath $vst3Dir -Recurse -Filter "*.vst3" -File | Select-Object -First 1
$pluginVersion = if ($vst3Dll) { $vst3Dll.VersionInfo.FileVersion } else { "(not built)" }
Write-Host "    app $exeVersion, plug-in $pluginVersion"
if ("$exeVersion".Trim() -ne $version -or "$pluginVersion".Trim() -ne $version) {
    throw "version resources (app $exeVersion, plug-in $pluginVersion) differ from Version.h $version"
}

Write-Host "==> Stage the service"
Invoke-Checked $Python @((Join-Path $Root "scripts\stage_local_services.py"), "--check")
Invoke-Checked $Python @((Join-Path $Root "scripts\scan_release_secrets.py"), (Join-Path $BuildDir "packaging\local-services"))

# Signing.
$signCommand = $env:KHENSU_SIGN_COMMAND
$signArgs = @()
if (-not $signCommand) {
    if ($env:KHENSU_SIGN_PFX) {
        if (-not $env:KHENSU_SIGN_PFX_PASSWORD) { throw "KHENSU_SIGN_PFX is set but KHENSU_SIGN_PFX_PASSWORD is not" }
        $signArgs = @("-PfxPath", $env:KHENSU_SIGN_PFX)
        $signCommand = 'signtool sign /fd sha256 /tr http://timestamp.digicert.com /td sha256 /f $q' +
            $env:KHENSU_SIGN_PFX + '$q /p $q' + $env:KHENSU_SIGN_PFX_PASSWORD + '$q $f'
    } elseif ($env:KHENSU_SIGN_THUMBPRINT) {
        $signArgs = @("-Thumbprint", $env:KHENSU_SIGN_THUMBPRINT)
        $signCommand = 'signtool sign /fd sha256 /tr http://timestamp.digicert.com /td sha256 /sha1 ' +
            $env:KHENSU_SIGN_THUMBPRINT + ' $f'
    }
}
$signed = [bool]$signCommand
if ($signed) {
    if ($signArgs.Count -gt 0) {
        Write-Host "==> Sign the app and plug-ins"
        Invoke-Checked "pwsh" (@("-NoProfile", "-File", (Join-Path $Root "packaging\sign-artifacts.ps1"), "-Require",
            "-BuildDir", $BuildDir) + $signArgs)
    }
} elseif ($RequireSigned) {
    throw "-RequireSigned: no certificate configured (KHENSU_SIGN_PFX, KHENSU_SIGN_THUMBPRINT or KHENSU_SIGN_COMMAND)"
} else {
    Write-Host "==> No certificate configured: the installer is built UNSIGNED (SmartScreen will warn)"
}

Write-Host "==> Installer"
$iscc = @(
    (Get-Command ISCC.exe -ErrorAction SilentlyContinue | ForEach-Object Source),
    (Join-Path $env:LOCALAPPDATA "Programs\Inno Setup 6\ISCC.exe"),
    (Join-Path ${env:ProgramFiles(x86)} "Inno Setup 6\ISCC.exe"),
    (Join-Path $env:ProgramFiles "Inno Setup 6\ISCC.exe")
) | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -First 1
if (-not $iscc) { throw "Inno Setup 6 (ISCC.exe) not found" }
$out = Join-Path $Root ".build-artifacts\release-$version"
New-Item -ItemType Directory -Force -Path $out | Out-Null
$isccArgs = @("/Q", "/DBuildDir=$BuildDir", "/O$out", "/Fkhensu-setup-$version")
if ($signed) { $isccArgs += @("/DSignToolName=khensu", "/Skhensu=$signCommand") }
Invoke-Checked $iscc ($isccArgs + @((Join-Path $Root "packaging\khensu-installer.iss")))

$setup = Join-Path $out "khensu-setup-$version.exe"
$setupVersion = (Get-Item -LiteralPath $setup).VersionInfo.ProductVersion
if ("$setupVersion".Trim() -ne $version) { throw "installer product version $setupVersion differs from $version" }
if ($signed) {
    $sig = Get-AuthenticodeSignature -LiteralPath $setup
    if ($sig.Status -ne "Valid") { throw "installer signature is $($sig.Status)" }
}
$hash = (Get-FileHash -LiteralPath $setup -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText((Join-Path $out "SHA256SUMS.txt"), "$hash  khensu-setup-$version.exe`n")
Write-Host ("==> {0} ({1:N0} MB, {2})" -f $setup, ((Get-Item -LiteralPath $setup).Length / 1MB), $(if ($signed) { "signed" } else { "unsigned" }))
Write-Host "    SHA-256 $hash"
Write-Host "    Release notes: write $out\notes.md, then: gh release create v$version -R KhensuLabs/khensu-mixing ..."
