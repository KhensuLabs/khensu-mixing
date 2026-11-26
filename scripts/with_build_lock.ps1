<#
.SYNOPSIS
  Runs a command while holding Global\KhensuBuildLock, the same named mutex
  verify_all.ps1 takes around builds. Use it whenever several people or agents
  share one tree: C++ builds, C++ test runs and the API generators
  (export_openapi.py / gen_cpp_api.py) must never overlap.

  Both waits have a limit (VER-1), set through the environment so that no
  option of this script can be mistaken for one of the command's:

    KHENSU_LOCK_WAIT_MIN      how long to wait for the lock   (default 90)
    KHENSU_BUILD_TIMEOUT_MIN  how long the command may run    (default 120)

  0 turns a limit off. A wait that gives up says who holds the lock; a
  command past its limit is killed with its child processes, the lock is
  released and the exit code is 124.

.EXAMPLE
  pwsh scripts/with_build_lock.ps1 cmake --build build --config Release --parallel 4 --target KhensuDesktop
#>
param([Parameter(Mandatory, ValueFromRemainingArguments)][string[]]$Command)

. (Join-Path $PSScriptRoot "lib\ProcessDeadline.ps1")

function Get-MinutesSetting([string]$name, [double]$default) {
    $raw = [Environment]::GetEnvironmentVariable($name)
    $value = 0.0
    if ($raw -and [double]::TryParse($raw, [ref]$value) -and $value -ge 0) { return $value }
    return $default
}
$lockWaitSec = 60 * (Get-MinutesSetting "KHENSU_LOCK_WAIT_MIN" 90)
$commandSec = 60 * (Get-MinutesSetting "KHENSU_BUILD_TIMEOUT_MIN" 120)

# MSBuild's reusable worker nodes outlive the build that started them and are
# not its children, so a kill at the deadline would miss them. Keep each
# build's nodes in its own process tree.
$env:MSBUILDDISABLENODEREUSE = "1"

# PowerShell reads a "-name:value" token as parameter syntax and hands the
# script "-name:" and "value" separately, so `--screenshot=C:/dir` arrived as
# `--screenshot=C:` + `/dir` (the app then wrote into a relative "C" folder)
# and `--fix=D:/x.wav` as two broken arguments. Rejoin them.
$joined = [System.Collections.Generic.List[string]]::new()
for ($i = 0; $i -lt $Command.Count; $i++) {
    $tok = $Command[$i]
    $next = if (($i + 1) -lt $Command.Count) { $Command[$i + 1] } else { $null }
    if ($null -ne $next -and $tok -match '^-.*:$') {
        $joined.Add($tok + $next); $i++
    } elseif ($null -ne $next -and $tok -match '^-.*=[A-Za-z]$' -and $next -match '^[\\/]') {
        # the colon itself is eaten: "--out=C" + "/dir" was "--out=C:/dir"
        $joined.Add($tok + ':' + $next); $i++
    } else {
        $joined.Add($tok)
    }
}
$Command = $joined.ToArray()

$exe = $Command[0]
$rest = @()
if ($Command.Count -gt 1) { $rest = $Command[1..($Command.Count - 1)] }

Write-Host "waiting for the build lock..."
try {
    $mutex = Enter-BuildLock -TimeoutSec $lockWaitSec -Command ($Command -join " ")
} catch [StepTimeout] {
    [Console]::Error.WriteLine("with_build_lock: $($_.Exception.Message)")
    exit 124
}
$code = 1
try {
    $code = Invoke-WithDeadline -Exe $exe -Arguments $rest -TimeoutSec $commandSec -What $exe
} catch [StepTimeout] {
    [Console]::Error.WriteLine("with_build_lock: $($_.Exception.Message)")
    $code = 124
} finally {
    Exit-BuildLock -Mutex $mutex
}
exit $code
