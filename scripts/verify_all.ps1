<#
.SYNOPSIS
    Local stand-in for hosted CI: runs every gate CI would, plus the Debug
    OutputDebugString sweep, and prints a PASS/FAIL summary.

.DESCRIPTION
    Hosted CI has been blocked since 2026-07-24 (CFG-01), so this script is the
    project's CI until it is restored. Steps, in order:

      lock-sync      scripts/check_venv_lock_sync.py: installed .venv versions
                     vs uv.lock (base + dev); lists the drift and WARNs, never
                     fails (PKG-2)
      doc-drift      scripts/check_doc_drift.py                      (fast, blocking)
      i18n           scripts/extract_strings.py --check              (fast, blocking)
      ruff           ruff check local-services                       (fast, blocking; skipped if ruff absent)
      script-tests   scripts/tests/*.ps1: the deadline and build-lock helpers
                     this script depends on
      cpp-build      Release build of the app, the VST3 and the 3 Catch2 suites
      cpp-tests      KhensuCoreTests, KhensuTests, KhensuPluginTests (Release)
      real-plugins   the two hidden tests that host real VST3s (desktop chain
                     "[.integration]", plugin Hosted tab "[hosting-real]",
                     VER-6); plugins from KHENSU_TEST_VST3 or a default set.
                     SKIP when neither finds a plugin
      app-smoke      the built app with --ui-bench: constructs the whole
                     AppController, visits every screen and tears it down;
                     fails on a crash, a non-zero exit or a screen over its
                     layout budget (ENG-2's construct/destroy check)
      app-loop       the built app with --screenshot-audio on -LoopAudio
                     (default scratch_audio/sample.wav; KHENSU_LOOP_AUDIO):
                     analyse, fix the top issue, the fix check, an Intake
                     drop; scripts/check_gate_log.py asserts the log (VER-3).
                     WARN when only stem separation is unavailable; SKIP
                     without an audio file. The app starts the service itself.
      pytest         full local-services suite, own --basetemp
      clap-e2e       scripts/check_clap_end_to_end.py: the pinned CLAP model
                     built, tokenised and run on the venv's NumPy (PKG-5:
                     laion-clap declares numpy<2); SKIP without laion-clap or
                     its cached weights
      pluginval      strictness 10 on the built VST3; fixed order blocking,
                     and a --randomise pass, both blocking (v1.0.4; v1.0.3's
                     host flaked "HPF On not restored" under random order)
      debug-capture  Debug build + every test in its own process with
                     OutputDebugString captured (scripts/dbwin_capture.py);
                     fails on any JUCE assertion, leak report or failure
      bad-files      the hostile-input sweep (C++ [bad_input] + pytest bad-audio
                     modules); skipped as redundant when cpp-tests and pytest
                     both ran, unless selected with -Only
      asan           AddressSanitizer build + run of the 3 suites; skipped with a
                     message when the MSVC ASan runtime is not installed
      bench          performance benchmarks if a runner exists (non-blocking)
      installer-stage  scripts/stage_local_services.py --check: stages the
                     service the installer ships and fails on debris (P0-3)
      installer-upgrade  STR-3: installs the published v1.0.0 installer
                     (app only, per user, into the log folder), runs this
                     build's installer over it with the arguments the in-app
                     updater passes (UpdateInstaller.cpp: no /DIR, no
                     /COMPONENTS), checks it upgraded in place - same folder,
                     this build's exe, one uninstall entry, no plug-ins added,
                     the user's config dir untouched - then uninstalls.
                     Uses the real AppId: SKIP when Khensu is installed here.
      installer-smoke  compiles packaging/khensu-installer.iss (own AppId),
                     installs it silently into the log folder with only the
                     app component, checks the files that must and must not
                     be there, uninstalls it and checks nothing is left
                     (SHIP-1). Without a staged Python runtime it builds with
                     /DNoRuntime and says so; SKIP without Inno Setup 6.

    -Fast runs only doc-drift, i18n and ruff - what .githooks/pre-push runs.
    C++ builds take the same named mutex (Global\KhensuBuildLock) as the
    agent build wrapper, so two builds never share a tree at once.

    Every step has a deadline (VER-1). A step that runs past it has its
    process tree killed and is reported as FAIL with "deadline" in the note,
    and the run carries on with the next step. The wait for the build lock is
    limited too (-LockWaitMinutes) and a wait that gives up names the holder.
    Time spent waiting for the lock does not count against the step.
    -TimeoutScale 2 doubles every deadline (a slow machine, a cold build);
    -TimeoutScale 0 turns the deadlines off.

.EXAMPLE
    pwsh scripts/verify_all.ps1                   # everything
    pwsh scripts/verify_all.ps1 -Fast             # pre-push gates
    pwsh scripts/verify_all.ps1 -Only cpp-build,cpp-tests
    pwsh scripts/verify_all.ps1 -Skip debug-capture,asan -DryRun
    pwsh scripts/verify_all.ps1 -Only debug-capture -TimeoutScale 2
    pwsh scripts/verify_all.ps1 -Only app-loop -LoopAudio D:\music\mix.wav
#>
param(
    [switch]$Fast,
    [string[]]$Only = @(),
    [string[]]$Skip = @(),
    [switch]$DryRun,
    [string]$BuildDir = "",
    [string]$DebugBuildDir = "",
    [string]$AsanBuildDir = "",
    [string]$Pluginval = "",
    [int]$Parallel = 4,
    [string]$LogDir = "",
    [double]$TimeoutScale = 1.0,
    [double]$LockWaitMinutes = 90,
    [string]$LoopAudio = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot "lib\ProcessDeadline.ps1")

# MSBuild keeps idle worker nodes alive and hands them to the next build.
# Those are not children of that build, so killing its process tree at a
# deadline would leave them compiling. Fresh nodes per build stay in the tree.
$env:MSBUILDDISABLENODEREUSE = "1"

$Root = Split-Path -Parent $PSScriptRoot
$ServiceRoot = Join-Path $Root "local-services"
if (-not $BuildDir) { $BuildDir = Join-Path $Root "build" }
if (-not $DebugBuildDir) {
    $DebugBuildDir = if ($env:KHENSU_DEBUG_BUILD_DIR) { $env:KHENSU_DEBUG_BUILD_DIR } else { "D:\khensu-build-debug" }
}
if (-not $AsanBuildDir) {
    $AsanBuildDir = if ($env:KHENSU_ASAN_BUILD_DIR) { $env:KHENSU_ASAN_BUILD_DIR } else { Join-Path (Split-Path -Parent $DebugBuildDir) "khensu-build-asan" }
}
if (-not $LogDir) { $LogDir = Join-Path $BuildDir ("verify-logs\" + (Get-Date -Format "yyyyMMdd-HHmmss")) }

$AllSteps = @("lock-sync", "doc-drift", "i18n", "ruff", "script-tests", "cpp-build", "cpp-tests", "real-plugins", "app-smoke", "app-loop", "pytest",
              "clap-e2e", "pluginval", "debug-capture", "bad-files", "asan", "bench", "installer-stage", "installer-smoke",
              "installer-upgrade")
$FastSteps = @("doc-drift", "i18n", "ruff")
$TestExes = @("KhensuCoreTests", "KhensuTests", "KhensuPluginTests")
$PluginvalVersion = "v1.0.4"
# SHA-256 of that release's pluginval_Windows.zip (SHIP-5); CI checks the same hash.
$PluginvalZipSha256 = "c08e61ce3b96db41636f8ec7e76f4c7e2c13ebdac7fa1b5a1f52b4f32ec715ab"
$Vst3Rel = "plugin-copilot\KhensuCopilot_artefacts\Release\VST3\Khensu Copilot.vst3"

# Accept "-Only a,b" as well as "-Only a -Only b" / comma-joined strings.
$Only = @($Only | ForEach-Object { $_ -split "," } | Where-Object { $_ })
$Skip = @($Skip | ForEach-Object { $_ -split "," } | Where-Object { $_ })
foreach ($name in $Only + $Skip) {
    if ($AllSteps -notcontains $name) { throw "Unknown step '$name'. Steps: $($AllSteps -join ', ')" }
}
$Selected = if ($Only.Count) { $Only } elseif ($Fast) { $FastSteps } else { $AllSteps }
$Selected = @($AllSteps | Where-Object { $Selected -contains $_ -and $Skip -notcontains $_ })

# ---------------------------------------------------------------- helpers ---

$Python = Join-Path $ServiceRoot ".venv\Scripts\python.exe"
if (-not (Test-Path -LiteralPath $Python)) { $Python = Join-Path $ServiceRoot ".venv/bin/python" }
if (-not (Test-Path -LiteralPath $Python)) {
    $cmd = Get-Command python -ErrorAction SilentlyContinue
    $Python = if ($cmd) { $cmd.Source } else { "" }
}

class StepSkipped : System.Exception { StepSkipped([string]$m) : base($m) {} }
class StepWarning : System.Exception { StepWarning([string]$m) : base($m) {} }

function Skip-Step([string]$why) { throw [StepSkipped]::new($why) }
function Warn-Step([string]$why) { throw [StepWarning]::new($why) }

# The running step's deadline. Every command a step starts gets what is left
# of it, so a step as a whole cannot outlive its budget.
$script:StepName = ""
$script:StepDeadline = $null
$script:StepBudgetSec = 0.0

# Seconds left for the running step (0 = no limit). Throws once it is spent.
function Get-StepSecondsLeft {
    if ($null -eq $script:StepDeadline) { return 0.0 }
    $left = ($script:StepDeadline - (Get-Date)).TotalSeconds
    if ($left -le 1) {
        throw [StepTimeout]::new("$($script:StepName) used up its $(Format-Duration $script:StepBudgetSec) deadline")
    }
    return $left
}

# The limit for one command: what is left of the step, or $maxMinutes (scaled)
# when that is shorter. 0 = no limit.
function Get-CommandLimit([double]$maxMinutes = 0) {
    $left = Get-StepSecondsLeft
    if ($maxMinutes -le 0 -or $TimeoutScale -le 0) { return $left }
    $cap = $maxMinutes * 60 * $TimeoutScale
    if ($left -le 0) { return $cap }
    return [math]::Min($left, $cap)
}

# Runs a native command (or prints it under -DryRun) and throws on a non-zero
# exit. The command is killed, with its children, at the step's deadline, or
# after $maxMinutes when that comes first.
function Invoke-Native([string]$exe, [string[]]$arguments, [string]$workDir = $Root, [double]$maxMinutes = 0) {
    $shown = ($arguments | ForEach-Object { if ($_ -match '\s') { "`"$_`"" } else { $_ } }) -join " "
    if ($DryRun) { Write-Host "    [dry-run] (cd $workDir) $exe $shown"; return }
    $name = [IO.Path]::GetFileName($exe)
    $code = Invoke-WithDeadline -Exe $exe -Arguments $arguments -WorkDir $workDir `
        -TimeoutSec (Get-CommandLimit $maxMinutes) -What "$($script:StepName): $name"
    if ($code -ne 0) { throw "$name exited $code" }
}

# Runs a native command and returns its exit code and output lines instead of
# throwing on a non-zero exit. Same deadline rules as Invoke-Native.
function Invoke-NativeCapture([string]$exe, [string[]]$arguments, [string]$workDir = $Root, [double]$maxMinutes = 0) {
    return Invoke-WithDeadline -Exe $exe -Arguments $arguments -WorkDir $workDir -Capture `
        -TimeoutSec (Get-CommandLimit $maxMinutes) -What "$($script:StepName): $([IO.Path]::GetFileName($exe))"
}

function Require-Python {
    if (-not $Python) { throw "No Python found (expected local-services\.venv)." }
}

# Holds Global\KhensuBuildLock around a build so parallel builds never share a tree.
function Invoke-LockedBuild([string]$tree, [string]$config, [string[]]$targets) {
    if (-not $DryRun -and -not (Test-Path -LiteralPath (Join-Path $tree "CMakeCache.txt"))) {
        throw "No configured CMake tree at $tree"
    }
    $arguments = @("--build", $tree, "--config", $config, "--parallel", "$Parallel", "--target") + $targets
    if ($DryRun) { Invoke-Native "cmake" $arguments; return }
    Write-Host "    waiting for the build lock..."
    $waited = [System.Diagnostics.Stopwatch]::StartNew()
    $lockWaitSec = if ($TimeoutScale -le 0) { 0 } else { $LockWaitMinutes * 60 * $TimeoutScale }
    $mutex = Enter-BuildLock -TimeoutSec $lockWaitSec -Command "verify_all $($script:StepName): cmake $($arguments -join ' ')"
    $waited.Stop()
    # Waiting for someone else's build is not this step's time.
    if ($null -ne $script:StepDeadline) { $script:StepDeadline = $script:StepDeadline.AddSeconds($waited.Elapsed.TotalSeconds) }
    try { Invoke-Native "cmake" $arguments } finally { Exit-BuildLock -Mutex $mutex }
}

function Find-Pluginval {
    if ($Pluginval) { return $Pluginval }
    if ($env:KHENSU_PLUGINVAL) { return $env:KHENSU_PLUGINVAL }
    # Deliberately not `pluginval` from PATH: an older build there (v1.0.3)
    # reintroduces the randomised-order flake the pin exists to avoid.
    $cacheDir = Join-Path $env:LOCALAPPDATA "KhensuMixing\tools\pluginval-$PluginvalVersion"
    $cached = Join-Path $cacheDir "pluginval.exe"
    if (Test-Path -LiteralPath $cached) { return $cached }
    if ($DryRun) { return "$cached (would download $PluginvalVersion)" }
    # Same pinned release CI downloads (.github/workflows/ci.yml).
    Write-Host "    downloading pluginval $PluginvalVersion to $cacheDir"
    New-Item -ItemType Directory -Force -Path $cacheDir | Out-Null
    $zip = Join-Path $cacheDir "pluginval.zip"
    Invoke-WebRequest -Uri "https://github.com/Tracktion/pluginval/releases/download/$PluginvalVersion/pluginval_Windows.zip" -OutFile $zip
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $zip).Hash.ToLowerInvariant()
    if ($hash -ne $PluginvalZipSha256) {
        Remove-Item -LiteralPath $zip -Force
        throw "pluginval.zip SHA-256 $hash does not match the pin $PluginvalZipSha256"
    }
    Expand-Archive -Path $zip -DestinationPath $cacheDir -Force
    $exe = Get-ChildItem -Path $cacheDir -Recurse -Filter pluginval.exe | Select-Object -First 1
    if (-not $exe) { throw "pluginval.exe not found in the downloaded archive" }
    return $exe.FullName
}

# pluginval.exe is a GUI-subsystem binary: '&' would not wait for it, so it is
# started as a process, waited on (with a deadline) and its output captured.
function Invoke-Pluginval([string]$exe, [string[]]$extra, [string]$tag) {
    $vst3 = Join-Path $BuildDir $Vst3Rel
    $arguments = @("--strictness-level", "10", "--skip-gui-tests", "--validate-in-process") + $extra +
                 @("--validate", $vst3)
    if ($DryRun) { Write-Host "    [dry-run] $exe $($arguments -join ' ')"; return 0 }
    if (-not (Test-Path -LiteralPath $vst3)) { throw "Built VST3 not found at $vst3 (run cpp-build)" }
    $out = Join-Path $LogDir "pluginval-$tag-out.txt"
    $result = Invoke-NativeCapture $exe $arguments $Root 20
    $result.Output | Set-Content -LiteralPath $out -Encoding utf8
    $result.Output | Select-String -Pattern "FAILED|SUCCESS|Random seed|!!!" | ForEach-Object { Write-Host "    $($_.Line)" }
    return $result.ExitCode
}

function Find-AsanRuntime {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path -LiteralPath $vswhere)) { return $null }
    foreach ($install in (& $vswhere -all -products * -property installationPath)) {
        $dll = Get-ChildItem -Path (Join-Path $install "VC\Tools\MSVC") -Recurse `
            -Filter "clang_rt.asan_dynamic-x86_64.dll" -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($dll) { return $dll }
    }
    return $null
}

# ------------------------------------------------------------------ steps ---

$Results = [System.Collections.Generic.List[object]]::new()
function Get-StepStatus([string]$name) {
    $r = $Results | Where-Object { $_.Step -eq $name } | Select-Object -First 1
    if ($r) { return $r.Status } else { return "" }
}

$Steps = [ordered]@{
    "lock-sync" = @{ Blocking = $false; Minutes = 5; Run = {
        # Preflight (PKG-2): a .venv that drifted from uv.lock tests a different
        # stack from the one that ships. Report the drift, never block on it.
        Require-Python
        if ($DryRun) { Invoke-Native $Python @("scripts/check_venv_lock_sync.py"); return }
        # MAC-1: a runtime package without a wheel on a shipped platform fails
        # that platform's first run (no compiler on a user's Mac).
        foreach ($plat in @("macos-x86_64", "macos-arm64", "win-amd64")) {
            $wheels = Invoke-NativeCapture $Python @("scripts/check_platform_wheels.py", "--platform", $plat)
            $wheels.Output | ForEach-Object { Write-Host "    $_" }
            if ($wheels.ExitCode -ne 0) { Warn-Step "a runtime package has no $plat wheel - listed above" }
        }
        $result = Invoke-NativeCapture $Python @("scripts/check_venv_lock_sync.py")
        $out = $result.Output
        $code = $result.ExitCode
        $out | ForEach-Object { Write-Host "    $_" }
        if ($code -eq 1) {
            $n = @($out | Where-Object { "$_" -match ' -> ' -and "$_" -notmatch 'locked -> installed' }).Count
            Warn-Step "$n package(s) differ from uv.lock - listed above"
        }
        if ($code -ne 0) { Warn-Step "lock-sync could not run (exit $code)" }
    } }
    "doc-drift" = @{ Blocking = $true; Minutes = 5; Run = {
        Require-Python
        Invoke-Native $Python @("scripts/check_doc_drift.py")
    } }
    "installer-stage" = @{ Blocking = $true; Minutes = 15; Run = {
        Require-Python
        Invoke-Native $Python @("scripts/stage_local_services.py", "--check")
    } }
    "installer-upgrade" = @{ Blocking = $true; Minutes = 30; Run = {
        $iscc = @(
            (Get-Command ISCC.exe -ErrorAction SilentlyContinue | ForEach-Object Source),
            (Join-Path $env:LOCALAPPDATA "Programs\Inno Setup 6\ISCC.exe"),
            (Join-Path ${env:ProgramFiles(x86)} "Inno Setup 6\ISCC.exe"),
            (Join-Path $env:ProgramFiles "Inno Setup 6\ISCC.exe")
        ) | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -First 1
        if (-not $iscc) { Skip-Step "Inno Setup 6 (ISCC.exe) not installed" }
        # The shipped AppId really ends in "}}": Inno escapes "{{" but not
        # "}}" (khensu-installer.iss), so v1.0.0's uninstall key is this.
        $appId = "{B3D0F7B4-6C1E-4E9A-9D2F-4B2C1A7E9F10}}_is1"
        $uninstKeys = @("HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\$appId",
                        "HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\$appId",
                        "HKLM:\Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\$appId")
        # The real AppId: never run over a real install on this machine.
        if ($uninstKeys | Where-Object { Test-Path $_ }) { Skip-Step "Khensu Mixing is installed on this machine" }
        $gh = Get-Command gh -ErrorAction SilentlyContinue
        if (-not $gh) { Skip-Step "gh CLI not installed (downloads the v1.0.0 installer)" }
        $out = Join-Path $LogDir "installer-upgrade"
        New-Item -ItemType Directory -Force $out | Out-Null
        $old = Join-Path $out "khensu-setup-1.0.0.exe"
        if (-not (Test-Path -LiteralPath $old)) {
            Invoke-Native $gh.Source @("release", "download", "v1.0.0", "-R", "KhensuLabs/khensu-mixing",
                                       "-p", "khensu-setup-1.0.0.exe", "-D", $out)
        }
        $defines = @("/Q", "/DBuildDir=$BuildDir", "/O$out", "/Fkhensu-upgrade")
        if (-not (Test-Path -LiteralPath (Join-Path $BuildDir "packaging\python-runtime"))) { $defines += "/DNoRuntime" }
        Invoke-Native $iscc ($defines + @((Join-Path $Root "packaging\khensu-installer.iss")))
        if ($DryRun) { return }
        # A short folder: the bundled runtime's deepest files pass MAX_PATH
        # under a long log folder (MoveFile code 3), as they would not under
        # the default install folder.
        $dir = Join-Path ([IO.Path]::GetTempPath()) "khu-upgrade"
        if (Test-Path -LiteralPath $dir) { Remove-Item -LiteralPath $dir -Recurse -Force }
        $configDir = Join-Path $env:APPDATA "KhensuMixing"
        # A running app or service keeps writing its db, logs and caches here,
        # so the check is what an installer could break: nothing removed, and
        # the user's settings unchanged.
        $configFiles = { if (Test-Path $configDir) { @(Get-ChildItem -LiteralPath $configDir -Recurse -File -Force |
            ForEach-Object FullName) } else { @() } }
        $settings = Join-Path $configDir "settings.json"
        $settingsHash = { if (Test-Path -LiteralPath $settings) { (Get-FileHash -LiteralPath $settings).Hash } else { "" } }
        $configBefore = & $configFiles
        $settingsBefore = & $settingsHash
        $userVst3 = Join-Path $env:LOCALAPPDATA "Programs\Common\VST3\Khensu Copilot.vst3"
        $vst3Before = Test-Path -LiteralPath $userVst3
        try {
            $p = Start-Process -FilePath $old -Wait -PassThru -ArgumentList @(
                "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/CURRENTUSER", "/NOICONS", "/TASKS=""""",
                "/COMPONENTS=""app""", "/DIR=""$dir""", "/LOG=""$(Join-Path $out 'install-1.0.0.log')""")
            if ($p.ExitCode -ne 0) { throw "v1.0.0 install exited $($p.ExitCode)" }
            $exe = Join-Path $dir "Khensu Mixing.exe"
            if (-not (Test-Path -LiteralPath $exe)) { throw "v1.0.0 did not install into $dir" }
            $oldHash = (Get-FileHash -LiteralPath $exe).Hash
            # Exactly what UpdateInstaller.cpp passes: the folder and the
            # components must come from the previous install.
            $p = Start-Process -FilePath (Join-Path $out "khensu-upgrade.exe") -Wait -PassThru -ArgumentList @(
                "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/SP-", "/CURRENTUSER",
                "/LOG=""$(Join-Path $out 'install-upgrade.log')""")
            if ($p.ExitCode -ne 0) { throw "upgrade install exited $($p.ExitCode) (see $out\install-upgrade.log)" }
            $built = Join-Path $BuildDir "desktop-app\KhensuDesktop_artefacts\Release\Khensu Mixing.exe"
            $newHash = (Get-FileHash -LiteralPath $exe).Hash
            if ($newHash -ne (Get-FileHash -LiteralPath $built).Hash) {
                throw "the upgrade did not replace the app in $dir (old $($oldHash.Substring(0,8)), now $($newHash.Substring(0,8)))"
            }
            $keys = @($uninstKeys | Where-Object { Test-Path $_ })
            if ($keys.Count -ne 1) { throw "expected one uninstall entry, found $($keys.Count)" }
            $loc = (Get-ItemProperty $keys[0]).InstallLocation.TrimEnd('\')
            if ($loc -ne $dir.TrimEnd('\')) { throw "the upgrade moved the install: $loc (was $dir)" }
            if (-not $vst3Before -and (Test-Path -LiteralPath $userVst3)) {
                throw "the upgrade added the VST3 plug-in the v1.0.0 install had left out"
            }
            $configAfter = & $configFiles
            $gone = @($configBefore | Where-Object { $configAfter -notcontains $_ -and $_ -notmatch '\\(logs|cache)\\|-wal$|-shm$|\.tmp$' })
            if ($gone) { throw "the installers removed $($gone.Count) file(s) from $configDir, e.g. $($gone[0])" }
            if ((& $settingsHash) -ne $settingsBefore) { throw "the installers changed $settings" }
            Write-Host "  v1.0.0 -> this build: upgraded in place ($dir), one uninstall entry, config untouched"
        } finally {
            $unins = Join-Path $dir "unins000.exe"
            if (Test-Path -LiteralPath $unins) {
                Start-Process -FilePath $unins -Wait -ArgumentList @("/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART") | Out-Null
                for ($i = 0; $i -lt 30 -and (Test-Path -LiteralPath (Join-Path $dir "Khensu Mixing.exe")); $i++) { Start-Sleep -Seconds 1 }
            }
        }
        if (Test-Path -LiteralPath (Join-Path $dir "Khensu Mixing.exe")) { throw "uninstall left the app behind in $dir" }
        if ($uninstKeys | Where-Object { Test-Path $_ }) { throw "uninstall left its uninstall entry behind" }
    } }
    "installer-smoke" = @{ Blocking = $true; Minutes = 20; Run = {
        $iscc = @(
            (Get-Command ISCC.exe -ErrorAction SilentlyContinue | ForEach-Object Source),
            (Join-Path $env:LOCALAPPDATA "Programs\Inno Setup 6\ISCC.exe"),
            (Join-Path ${env:ProgramFiles(x86)} "Inno Setup 6\ISCC.exe"),
            (Join-Path $env:ProgramFiles "Inno Setup 6\ISCC.exe")
        ) | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -First 1
        if (-not $iscc) { Skip-Step "Inno Setup 6 (ISCC.exe) not installed" }
        Require-Python
        # The installer takes the service from the stage, never the dev tree.
        Invoke-Native $Python @("scripts/stage_local_services.py", "--check")
        $out = Join-Path $LogDir "installer-smoke"
        $defines = @("/Q", "/DMyAppId=KhensuInstallerSmoke", "/DBuildDir=$BuildDir", "/O$out", "/Fkhensu-smoke")
        if (-not (Test-Path -LiteralPath (Join-Path $BuildDir "packaging\python-runtime"))) {
            Write-Host "  no staged Python runtime (packaging/build-python-env.ps1): smoke installer built without it"
            $defines += "/DNoRuntime"
        }
        Invoke-Native $iscc ($defines + @((Join-Path $Root "packaging\khensu-installer.iss")))
        if ($DryRun) { return }
        $dir = Join-Path $out "app"
        # /CURRENTUSER and only the app: nothing lands in the user's real
        # plugin folders, Start menu or desktop.
        $install = Start-Process -FilePath (Join-Path $out "khensu-smoke.exe") -Wait -PassThru -ArgumentList @(
            "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/CURRENTUSER", "/NOICONS", "/TASKS=""""",
            "/COMPONENTS=""app""", "/DIR=""$dir""", "/LOG=""$(Join-Path $out 'install.log')""")
        if ($install.ExitCode -ne 0) { throw "silent install exited $($install.ExitCode) (see $out\install.log)" }
        $missing = @("Khensu Mixing.exe", "LICENSE.txt", "THIRD_PARTY_NOTICES.md", "resources\genre_norms", "local-services\launch_service.py",
                     "local-services\khensu_services\server.py", "unins000.exe") |
            Where-Object { -not (Test-Path -LiteralPath (Join-Path $dir $_)) }
        if ($missing) { throw "installed tree lacks: $($missing -join ', ')" }
        # The bundled Python runtime (local-services\runtime, from the hash-pinned
        # build-python-env.ps1) ships precompiled .pyc on purpose - an install
        # under Program Files can't write its own - and its packages' own test
        # folders; the rule is for everything else. desktop.ini still counts.
        $runtime = Join-Path $dir "local-services\runtime\"
        $debris = @(Get-ChildItem -LiteralPath $dir -Recurse -File -Force |
            Where-Object { $_.Name -match '^desktop\.ini$' -or (-not $_.FullName.StartsWith($runtime, [StringComparison]::OrdinalIgnoreCase) -and
                ($_.Name -match '\.(pdb|ilk|exp|lib|pyc)$|running-old' -or $_.FullName -match '\\(tests|__pycache__|\.pytest_tmp[^\\]*|\.venv[^\\]*)\\')) })
        if ($debris) { throw "installed tree has debris: $(($debris | Select-Object -First 5 | ForEach-Object Name) -join ', ')" }
        # v1.0.0: what users get carries no keys, tokens or personal data.
        Invoke-Native $Python @("scripts/scan_release_secrets.py", $dir)
        Write-Host ("  installed {0} files, {1:N0} MB" -f @(Get-ChildItem -LiteralPath $dir -Recurse -File).Count,
            ((Get-ChildItem -LiteralPath $dir -Recurse -File | Measure-Object Length -Sum).Sum / 1MB))
        $uninstall = Start-Process -FilePath (Join-Path $dir "unins000.exe") -Wait -PassThru `
            -ArgumentList @("/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART")
        if ($uninstall.ExitCode -ne 0) { throw "silent uninstall exited $($uninstall.ExitCode)" }
        # The uninstaller re-launches itself from %TEMP% and removes the
        # folder after the first copy exits: give it a moment.
        for ($i = 0; $i -lt 30 -and (Test-Path -LiteralPath (Join-Path $dir "Khensu Mixing.exe")); $i++) { Start-Sleep -Seconds 1 }
        if (Test-Path -LiteralPath (Join-Path $dir "Khensu Mixing.exe")) { throw "uninstall left the app behind in $dir" }
    } }
    "i18n" = @{ Blocking = $true; Minutes = 5; Run = {
        Require-Python
        Invoke-Native $Python @("scripts/extract_strings.py", "--check")
    } }
    "ruff" = @{ Blocking = $true; Minutes = 5; Run = {
        Require-Python
        # Like the pre-push hook: a contributor without the dev extra is not blocked.
        if ($DryRun) { Invoke-Native $Python @("-m", "ruff", "check", ".") $ServiceRoot; return }
        $probe = Invoke-NativeCapture $Python @("-c", "import ruff")
        if ($probe.ExitCode -ne 0) {
            $global = Get-Command ruff -ErrorAction SilentlyContinue
            if (-not $global) { Skip-Step "ruff not installed (pip install -e .[dev])" }
            Invoke-Native $global.Source @("check", ".") $ServiceRoot
        } else {
            Invoke-Native $Python @("-m", "ruff", "check", ".") $ServiceRoot
        }
    } }
    "script-tests" = @{ Blocking = $true; Minutes = 5; Run = {
        $tests = @(Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot "tests") -Filter "test_*.ps1" | Sort-Object Name)
        if (-not $tests.Count) { Skip-Step "no scripts/tests/test_*.ps1" }
        foreach ($t in $tests) { Invoke-Native "pwsh" @("-NoProfile", "-File", $t.FullName) }
    } }
    "cpp-build" = @{ Blocking = $true; Minutes = 90; Run = {
        Invoke-LockedBuild $BuildDir "Release" (@("KhensuDesktop", "KhensuCopilot_VST3") + $TestExes)
    } }
    "cpp-tests" = @{ Blocking = $true; Minutes = 30; Run = {
        $failed = @()
        foreach ($t in $TestExes) {
            $exe = Join-Path $BuildDir "tests\Release\$t.exe"
            if (-not $DryRun -and -not (Test-Path -LiteralPath $exe)) { throw "$exe not built (run cpp-build)" }
            try { Invoke-Native $exe @("--reporter", "compact") $Root 15 } catch { $failed += "$t ($($_.Exception.Message))" }
        }
        if ($failed) { throw "failing suites: $($failed -join '; ')" }
    } }
    "real-plugins" = @{ Blocking = $true; Minutes = 15; Run = {
        $runs = @(
            @{ Exe = "KhensuTests"; Filter = "[.integration]" },
            @{ Exe = "KhensuPluginTests"; Filter = "[hosting-real]" }
        )
        $failed = @(); $skipped = 0
        foreach ($r in $runs) {
            $exe = Join-Path $BuildDir "tests\Release\$($r.Exe).exe"
            if ($DryRun) { Write-Host "    [dry-run] $exe $($r.Filter)"; continue }
            if (-not (Test-Path -LiteralPath $exe)) { throw "$exe not built (run cpp-build)" }
            $result = Invoke-NativeCapture $exe @($r.Filter, "--reporter", "compact") $Root 10
            $result.Output | Set-Content -LiteralPath (Join-Path $LogDir "real-plugins-$($r.Exe).txt") -Encoding utf8
            $summary = ($result.Output | Select-Object -Last 3) -join " "
            Write-Host "    $($r.Exe) $($r.Filter): $summary"
            # Catch2 reports a SKIP() (no test plugin found) as skipped, exit 4.
            if ($summary -match "skipped" -and $summary -notmatch "failed") { $skipped++ }
            elseif ($result.ExitCode -ne 0) { $failed += "$($r.Exe) exited $($result.ExitCode)" }
        }
        if ($failed) { throw ($failed -join "; ") }
        if (-not $DryRun -and $skipped -eq $runs.Count) { Skip-Step "no test plugins found (set KHENSU_TEST_VST3)" }
    } }
    "app-smoke" = @{ Blocking = $true; Minutes = 10; Run = {
        $app = Join-Path $BuildDir "desktop-app\KhensuDesktop_artefacts\Release\Khensu Mixing.exe"
        if (-not $DryRun -and -not (Test-Path -LiteralPath $app)) { throw "$app not built (run cpp-build)" }
        $bench = Join-Path $LogDir "app-smoke-ui-bench.json"
        if ($DryRun) { Write-Host "    [dry-run] `"$app`" --ui-bench=$bench"; return }
        # A GUI-subsystem exe: PowerShell doesn't wait for it or set
        # $LASTEXITCODE, so start it explicitly and read its exit code. The
        # raw command line keeps the quotes where the app expects them.
        $code = Invoke-WithDeadline -Exe $app -RawArguments "--ui-bench=`"$bench`"" -WorkDir $Root `
            -TimeoutSec (Get-CommandLimit) -What "app-smoke: --ui-bench"
        if ($code -ne 0) { throw "--ui-bench exited $code (see $bench)" }
    } }
    "app-loop" = @{ Blocking = $true; Minutes = 30; Run = {
        $app = Join-Path $BuildDir "desktop-app\KhensuDesktop_artefacts\Release\Khensu Mixing.exe"
        if (-not $DryRun -and -not (Test-Path -LiteralPath $app)) { throw "$app not built (run cpp-build)" }
        $audio = if ($LoopAudio) { $LoopAudio } elseif ($env:KHENSU_LOOP_AUDIO) { $env:KHENSU_LOOP_AUDIO } else { Join-Path $Root "scratch_audio\sample.wav" }
        if (-not (Test-Path -LiteralPath $audio -PathType Leaf)) {
            Skip-Step "no audio for the loop ($audio); pass -LoopAudio <wav> or set KHENSU_LOOP_AUDIO"
        }
        Require-Python
        $shots = Join-Path $LogDir "app-loop"
        if ($DryRun) { Write-Host "    [dry-run] `"$app`" --screenshot=$shots --screenshot-audio=$audio"; return }
        New-Item -ItemType Directory -Force -Path $shots | Out-Null
        # A GUI exe (see app-smoke). It exits 1 when stems are unavailable,
        # so the log, not the exit code, decides.
        $code = Invoke-WithDeadline -Exe $app -RawArguments "--screenshot=`"$shots`" --screenshot-audio=`"$audio`"" `
            -WorkDir $Root -TimeoutSec (Get-CommandLimit) -What "app-loop: --screenshot-audio"
        $verdict = Invoke-NativeCapture $Python @("scripts/check_gate_log.py", (Join-Path $shots "analysis-gate-log.txt"))
        $verdict.Output | ForEach-Object { Write-Host "    $_" }
        $summary = ($verdict.Output | Where-Object { $_ -match '^(FAIL|WARN):' }) -join "; "
        if ($verdict.ExitCode -eq 2) { Warn-Step $summary }
        if ($verdict.ExitCode -ne 0) { throw "loop failed (app exit $code): $summary - see $shots" }
    } }
    "pytest" = @{ Blocking = $true; Minutes = 60; Run = {
        Require-Python
        # Own basetemp: pytest wipes it at start, so sharing one with another
        # concurrent run deletes that run's files mid-test.
        # BLD-4: in parallel when pytest-xdist is installed ([dev] extra), and the
        # 25 slowest tests recorded in bench-results/ so a creeping suite shows.
        $pyArgs = @("-m", "pytest", "-q", "--basetemp=.pytest_tmp_verify", "-p", "no:cacheprovider", "--durations=25")
        if ($DryRun) { Invoke-Native $Python ($pyArgs + @("-n", "auto", "--maxprocesses=4")) $ServiceRoot; return }
        $xdist = Invoke-NativeCapture $Python @("-c", "import xdist")
        # At most 4 workers: each holds decoded audio and its caches, and more ran
        # this machine out of memory beside a build.
        if ($xdist.ExitCode -eq 0) { $pyArgs += @("-n", "auto", "--maxprocesses=4") }
        $result = Invoke-NativeCapture $Python $pyArgs $ServiceRoot
        $result.Output | Set-Content -LiteralPath (Join-Path $LogDir "pytest.txt") -Encoding utf8
        $bench = Join-Path $Root "bench-results"
        New-Item -ItemType Directory -Force $bench | Out-Null
        $durations = $result.Output | Select-String -Pattern '^(=+ slowest|[0-9.]+s (call|setup|teardown))' | ForEach-Object { $_.Line }
        $durations | Set-Content -LiteralPath (Join-Path $bench ("pytest-durations-" + (Get-Date -Format "yyyy-MM-dd") + ".txt")) -Encoding utf8
        $result.Output | Select-Object -Last 3 | ForEach-Object { Write-Host "    $_" }
        if ($result.ExitCode -ne 0) { throw "pytest exit $($result.ExitCode) - see $LogDir\pytest.txt" }
    } }
    "clap-e2e" = @{ Blocking = $true; Minutes = 20; Run = {
        Require-Python
        $result = Invoke-NativeCapture $Python @("scripts/check_clap_end_to_end.py")
        $result.Output | Set-Content -LiteralPath (Join-Path $LogDir "clap-e2e.txt") -Encoding utf8
        if ($result.ExitCode -eq 2) { Skip-Step "CLAP not installed or its weights not cached" }
        if ($result.ExitCode -ne 0) { throw "check_clap_end_to_end.py exit $($result.ExitCode) - see $LogDir" }
    } }
    "pluginval" = @{ Blocking = $true; Minutes = 45; Run = {
        $exe = Find-Pluginval
        $code = Invoke-Pluginval $exe @() "fixed"
        if ($code -ne 0) { throw "pluginval (fixed order) exit $code - see $LogDir" }
        # v1.0.3's host never delivered fuzzed values without a process()
        # call, so its randomised order flaked "HPF On not restored"; v1.0.4
        # fixed that, and the randomised pass now blocks too.
        $rcode = Invoke-Pluginval $exe @("--randomise") "random"
        if ($rcode -ne 0) { throw "pluginval (--randomise) exit $rcode - see $LogDir" }
    } }
    "debug-capture" = @{ Blocking = $true; Minutes = 180; Run = {
        Require-Python
        Invoke-LockedBuild $DebugBuildDir "Debug" $TestExes
        $flagged = @()
        foreach ($t in $TestExes) {
            $exe = Join-Path $DebugBuildDir "tests\Debug\$t.exe"
            $report = Join-Path $LogDir "debug-capture-$t.txt"
            try {
                Invoke-Native $Python @("scripts/dbwin_capture.py", $exe, "--report", $report) $Root 40
            } catch { $flagged += "$t ($($_.Exception.Message))" }
        }
        if ($flagged) { throw "assertions/leaks/failures in: $($flagged -join '; ') - see $LogDir" }
    } }
    "bad-files" = @{ Blocking = $true; Minutes = 20; Run = {
        if (-not $Only.Count -and (Get-StepStatus "cpp-tests") -eq "PASS" -and (Get-StepStatus "pytest") -eq "PASS") {
            Skip-Step "covered by cpp-tests + pytest"
        }
        Require-Python
        $exe = Join-Path $BuildDir "tests\Release\KhensuCoreTests.exe"
        Invoke-Native $exe @("[bad_input]", "--reporter", "compact")
        Invoke-Native $Python @("-m", "pytest", "-q", "--basetemp=.pytest_tmp_verify_bad", "-p", "no:cacheprovider",
                                "tests/test_bad_audio_inputs.py", "tests/test_bad_audio_analyzers.py") $ServiceRoot
    } }
    "asan" = @{ Blocking = $true; Minutes = 240; Run = {
        $runtime = Find-AsanRuntime
        if (-not $runtime) {
            Skip-Step "MSVC AddressSanitizer runtime not installed (VS Installer > Individual components > 'C++ AddressSanitizer')"
        }
        if (-not $DryRun -and -not (Test-Path -LiteralPath (Join-Path $AsanBuildDir "CMakeCache.txt"))) {
            # Reuse the main tree's fetched sources so the ASan tree needs no network.
            $deps = Join-Path $BuildDir "_deps"
            Invoke-Native "cmake" @("-S", $Root, "-B", $AsanBuildDir, "-G", "Visual Studio 17 2022", "-A", "x64",
                # /RTC1 (CMake's Debug default) is incompatible with /fsanitize=address.
                "-DCMAKE_CXX_FLAGS=/DWIN32 /D_WINDOWS /GR /EHsc /fsanitize=address",
                "-DCMAKE_C_FLAGS=/DWIN32 /D_WINDOWS /fsanitize=address",
                # /Z7, not /Zi: no shared .pdb for parallel compilers to queue on (VER-2).
                "-DCMAKE_CXX_FLAGS_DEBUG=/Z7 /Ob0 /Od", "-DCMAKE_C_FLAGS_DEBUG=/Z7 /Ob0 /Od",
                "-DFETCHCONTENT_SOURCE_DIR_JUCE=$deps\juce-src",
                "-DFETCHCONTENT_SOURCE_DIR_CATCH2=$deps\catch2-src",
                "-DFETCHCONTENT_SOURCE_DIR_SQLITECPP=$deps\sqlitecpp-src")
        }
        Invoke-LockedBuild $AsanBuildDir "Debug" $TestExes
        # The ASan DLL must be on PATH for the instrumented exes to start.
        $env:PATH = "$($runtime.DirectoryName);$env:PATH"
        $failed = @()
        foreach ($t in $TestExes) {
            try { Invoke-Native (Join-Path $AsanBuildDir "tests\Debug\$t.exe") @("--reporter", "compact") $Root 40 }
            catch { $failed += "$t ($($_.Exception.Message))" }
        }
        if ($failed) { throw "ASan failures in: $($failed -join '; ')" }
    } }
    "bench" = @{ Blocking = $false; Minutes = 45; Run = {
        # A .ps1 runner wins, else a Python one. Individual bench scripts are
        # not run directly: the runner owns budgets and output.
        $candidates = @("scripts\bench\run_all.ps1", "scripts\run_benchmarks.ps1", "scripts\bench\run_benchmarks.ps1", "scripts\bench\run.ps1",
                        "scripts\bench\run_benchmarks.py", "scripts\bench\run_all.py")
        $runner = $candidates | ForEach-Object { Join-Path $Root $_ } | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
        if (-not $runner) { Skip-Step "no benchmark runner (looked for $($candidates -join ', '))" }
        if ($runner.EndsWith(".py")) {
            Require-Python
            Invoke-Native $Python @($runner)
        } else {
            Invoke-Native "pwsh" @("-NoProfile", "-File", $runner)
        }
    } }
}

# ------------------------------------------------------------------- main ---

if (-not $DryRun -and -not $Fast) {
    # Old basetemps, verify logs and git temp objects pile up in the synced tree (CLN-4).
    try { & (Join-Path $PSScriptRoot "prune_scratch.ps1") -BuildDir $BuildDir | Write-Host } catch { Write-Warning "prune_scratch: $_" }
    New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
}
$mode = if ($Fast) { "fast" } elseif ($Only.Count) { "only" } else { "full" }
Write-Host "verify_all ($mode$(if ($DryRun) { ', dry run' })): $($Selected -join ', ')"

foreach ($name in $Selected) {
    $step = $Steps[$name]
    Write-Host ""
    Write-Host "==> $name" -ForegroundColor Cyan
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $status = "PASS"; $note = ""
    $script:StepName = $name
    $script:StepBudgetSec = $step.Minutes * 60 * $TimeoutScale
    $script:StepDeadline = if ($TimeoutScale -gt 0) { (Get-Date).AddSeconds($script:StepBudgetSec) } else { $null }
    try {
        & $step.Run
    } catch [StepSkipped] {
        $status = "SKIP"; $note = $_.Exception.Message
    } catch [StepWarning] {
        $status = "WARN"; $note = $_.Exception.Message
    } catch {
        $status = if ($step.Blocking) { "FAIL" } else { "WARN" }
        $note = $_.Exception.Message
    }
    $sw.Stop()
    if ($DryRun -and $status -eq "PASS") { $status = "DRY" }
    $Results.Add([pscustomobject]@{ Step = $name; Status = $status; Seconds = [math]::Round($sw.Elapsed.TotalSeconds, 1); Note = $note })
    $colour = @{ PASS = "Green"; FAIL = "Red"; WARN = "Yellow"; SKIP = "DarkGray"; DRY = "DarkCyan" }[$status]
    Write-Host "<== $name $status ($([math]::Round($sw.Elapsed.TotalSeconds, 1)) s) $note" -ForegroundColor $colour
}

Write-Host ""
Write-Host "Summary" -ForegroundColor Cyan
$Results | Format-Table -AutoSize Step, Status, Seconds, Note | Out-String -Width 200 | Write-Host
if (-not $DryRun -and -not $Fast) { Write-Host "Logs: $LogDir" }

$blockingFailures = @($Results | Where-Object { $_.Status -eq "FAIL" })
if ($blockingFailures.Count) {
    Write-Host "verify_all: FAILED ($($blockingFailures.Step -join ', '))" -ForegroundColor Red
    exit 1
}
Write-Host "verify_all: OK" -ForegroundColor Green
exit 0
