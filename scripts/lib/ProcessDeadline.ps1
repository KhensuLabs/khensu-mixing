<#
.SYNOPSIS
  Deadlines for the local verification scripts (VER-1): run a process with a
  time limit that kills its whole tree, and take the shared build lock with a
  timed wait that says who holds it.

  Dot-source it: . (Join-Path $PSScriptRoot "lib\ProcessDeadline.ps1")

  Why: verify_all.ps1 and with_build_lock.ps1 had no timeout anywhere. One
  stuck cl.exe held a run, and the build lock every other build waits on, for
  about four hours (2026-11-09).
#>

Set-StrictMode -Version Latest

class StepTimeout : System.Exception { StepTimeout([string]$m) : base($m) {} }

$script:KhensuBuildLockName = "Global\KhensuBuildLock"

function Format-Duration([double]$seconds) {
    if ($seconds -ge 90) { return "{0:0.#} min" -f ($seconds / 60) }
    return "{0:0} s" -f $seconds
}

# Kills a process and everything it started. Best effort: a process that has
# already gone is not an error.
function Stop-ProcessTree([System.Diagnostics.Process]$Process) {
    try { $Process.Kill($true) } catch { }
    try { [void]$Process.WaitForExit(15000) } catch { }
}

# Waits for an already started process. Past the deadline its tree is killed
# and a StepTimeout is thrown. TimeoutSec <= 0 waits without a limit.
# Returns the exit code.
function Wait-ProcessOrKill {
    param(
        [Parameter(Mandatory)][System.Diagnostics.Process]$Process,
        [double]$TimeoutSec = 0,
        [string]$What = ""
    )
    if (-not $What) { $What = "process $($Process.Id)" }
    # Reading Handle keeps the exit code readable after the process ends.
    $null = $Process.Handle
    if ($TimeoutSec -le 0) {
        $Process.WaitForExit()
    } elseif (-not $Process.WaitForExit([int][math]::Min([double][int]::MaxValue, $TimeoutSec * 1000))) {
        Stop-ProcessTree $Process
        throw [StepTimeout]::new("$What exceeded its $(Format-Duration $TimeoutSec) deadline and was killed (with its child processes)")
    }
    # A second, untimed wait lets redirected output finish flushing.
    $Process.WaitForExit()
    return $Process.ExitCode
}

# Runs $Exe with $Arguments in $WorkDir and waits at most $TimeoutSec.
# By default the child shares this console, so its output appears as it is
# written. With -Capture the output is collected instead, and the result is
# an object with ExitCode and Output (stdout then stderr, as lines).
function Invoke-WithDeadline {
    param(
        [Parameter(Mandatory)][string]$Exe,
        [string[]]$Arguments = @(),
        # The command line exactly as given, for a program that parses it
        # itself (--flag="a path"). Use instead of -Arguments.
        [string]$RawArguments = "",
        [string]$WorkDir = "",
        [double]$TimeoutSec = 0,
        [switch]$Capture,
        [string]$What = ""
    )
    $resolved = $Exe
    if (-not (Test-Path -LiteralPath $Exe -PathType Leaf)) {
        $cmd = Get-Command $Exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
        if (-not $cmd) { throw "Command not found: $Exe" }
        $resolved = $cmd.Source
    }
    if (-not $What) { $What = [IO.Path]::GetFileName($resolved) }

    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $resolved
    if ($RawArguments) {
        if ($Arguments.Count) { throw "Invoke-WithDeadline: pass -Arguments or -RawArguments, not both" }
        $psi.Arguments = $RawArguments
    } else {
        foreach ($a in $Arguments) { $psi.ArgumentList.Add($a) }
    }
    $psi.WorkingDirectory = if ($WorkDir) { $WorkDir } else { (Get-Location).ProviderPath }
    $psi.UseShellExecute = $false
    if ($Capture) {
        $psi.RedirectStandardOutput = $true
        $psi.RedirectStandardError = $true
    }

    $proc = [System.Diagnostics.Process]::Start($psi)
    try {
        if ($Capture) {
            # Read both streams while waiting, or a full pipe blocks the child.
            $stdout = $proc.StandardOutput.ReadToEndAsync()
            $stderr = $proc.StandardError.ReadToEndAsync()
            $code = Wait-ProcessOrKill -Process $proc -TimeoutSec $TimeoutSec -What $What
            $text = $stdout.GetAwaiter().GetResult() + $stderr.GetAwaiter().GetResult()
            $lines = @($text -split "`r?`n" | Where-Object { $_ -ne "" })
            return [pscustomobject]@{ ExitCode = $code; Output = $lines }
        }
        return (Wait-ProcessOrKill -Process $proc -TimeoutSec $TimeoutSec -What $What)
    } finally {
        $proc.Dispose()
    }
}

function Get-BuildLockHolderFile([string]$Name) {
    $safe = ($Name -replace '[^A-Za-z0-9_.-]', '_')
    return (Join-Path ([IO.Path]::GetTempPath()) "$safe.holder.json")
}

# Describes who probably holds the lock: the holder note the owner wrote, and
# any compiler / build processes running now, oldest first.
function Get-BuildLockHolderText([string]$Name) {
    $parts = @()
    $file = Get-BuildLockHolderFile $Name
    if (Test-Path -LiteralPath $file) {
        try {
            $h = Get-Content -LiteralPath $file -Raw | ConvertFrom-Json
            $alive = [bool](Get-Process -Id $h.pid -ErrorAction SilentlyContinue)
            $state = if ($alive) { "still running" } else { "no longer running" }
            $parts += "held by PID $($h.pid) ($state) since $($h.started): $($h.command)"
        } catch { }
    }
    $names = @("cl", "link", "MSBuild", "cmake", "mspdbsrv")
    $procs = @(Get-Process -Name $names -ErrorAction SilentlyContinue | Sort-Object StartTime)
    if ($procs.Count) {
        $now = Get-Date
        $shown = $procs | Select-Object -First 8 | ForEach-Object {
            $age = try { Format-Duration ($now - $_.StartTime).TotalSeconds } catch { "?" }
            "$($_.ProcessName) PID $($_.Id) (running $age)"
        }
        $more = if ($procs.Count -gt 8) { " and $($procs.Count - 8) more" } else { "" }
        $parts += "build processes now: $($shown -join ', ')$more"
    }
    if (-not $parts.Count) { return "no holder note and no build processes found" }
    return ($parts -join "; ")
}

# Takes the build lock, waiting at most $TimeoutSec (<= 0: no limit). Returns
# the mutex; pass it to Exit-BuildLock. A timeout throws a StepTimeout that
# names the holder.
function Enter-BuildLock {
    param([double]$TimeoutSec = 0, [string]$Command = "", [string]$Name = $script:KhensuBuildLockName)
    $mutex = [System.Threading.Mutex]::new($false, $Name)
    $got = $false
    try {
        if ($TimeoutSec -le 0) { [void]$mutex.WaitOne(); $got = $true }
        else { $got = $mutex.WaitOne([int][math]::Min([double][int]::MaxValue, $TimeoutSec * 1000)) }
    } catch [System.Threading.AbandonedMutexException] {
        # The previous holder died without releasing: the lock is ours now.
        $got = $true
    }
    if (-not $got) {
        $who = Get-BuildLockHolderText $Name
        $mutex.Dispose()
        throw [StepTimeout]::new("waited $(Format-Duration $TimeoutSec) for the build lock and gave up - $who")
    }
    try {
        @{ pid = $PID; command = $Command; started = (Get-Date).ToString("yyyy-MM-dd HH:mm:ss") } |
            ConvertTo-Json -Compress | Set-Content -LiteralPath (Get-BuildLockHolderFile $Name) -Encoding utf8
    } catch { }
    return $mutex
}

function Exit-BuildLock {
    param([Parameter(Mandatory)][System.Threading.Mutex]$Mutex, [string]$Name = $script:KhensuBuildLockName)
    try {
        $file = Get-BuildLockHolderFile $Name
        if (Test-Path -LiteralPath $file) {
            $h = Get-Content -LiteralPath $file -Raw | ConvertFrom-Json
            if ($h.pid -eq $PID) { Remove-Item -LiteralPath $file -Force }
        }
    } catch { }
    try { $Mutex.ReleaseMutex() } finally { $Mutex.Dispose() }
}
