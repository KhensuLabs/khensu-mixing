<#
.SYNOPSIS
  Checks scripts/lib/ProcessDeadline.ps1 (VER-1): exit codes pass through, a
  process past its deadline is killed with its children, and a timed wait on a
  held build lock gives up and names the holder. Exits non-zero on a failure.

  pwsh scripts/tests/test_process_deadline.ps1
#>
$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest
. (Join-Path (Split-Path -Parent $PSScriptRoot) "lib\ProcessDeadline.ps1")

$failures = [System.Collections.Generic.List[string]]::new()
function Check([string]$name, [bool]$ok, [string]$detail = "") {
    if ($ok) { Write-Host "  ok    $name" }
    else { Write-Host "  FAIL  $name $detail" -ForegroundColor Red; $failures.Add($name) }
}
function Find-Marked([string]$marker) {
    @(Get-CimInstance Win32_Process -Filter "Name = 'pwsh.exe'" |
        Where-Object { $_.CommandLine -and $_.CommandLine.Contains($marker) -and $_.ProcessId -ne $PID })
}

# 1. The exit code comes back.
$code = Invoke-WithDeadline -Exe "pwsh" -Arguments @("-NoProfile", "-Command", "exit 3") -TimeoutSec 60
Check "exit code passes through" ($code -eq 3) "got $code"

# 2. Captured output and exit code.
$r = Invoke-WithDeadline -Exe "pwsh" -Arguments @("-NoProfile", "-Command", "Write-Output hello; exit 0") -TimeoutSec 60 -Capture
Check "capture returns output" ($r.ExitCode -eq 0 -and ($r.Output -contains "hello")) "got $($r.ExitCode) / $($r.Output -join '|')"

# 3. A process past its deadline is killed, and so is its child.
$marker = "khensu-deadline-test-" + [guid]::NewGuid().ToString("N")
$inner = "Start-Sleep 300 # $marker-child"
$outer = "pwsh -NoProfile -Command '$inner'; Start-Sleep 300 # $marker-parent"
$sw = [System.Diagnostics.Stopwatch]::StartNew()
$timedOut = $false; $message = ""
try {
    [void](Invoke-WithDeadline -Exe "pwsh" -Arguments @("-NoProfile", "-Command", $outer) -TimeoutSec 6 -What "sleeper")
} catch [StepTimeout] { $timedOut = $true; $message = $_.Exception.Message }
$sw.Stop()
Check "deadline throws StepTimeout" $timedOut
Check "timeout message names the step and the limit" ($message -match "sleeper" -and $message -match "6 s") $message
Check "returns soon after the deadline" ($sw.Elapsed.TotalSeconds -lt 40) "took $([int]$sw.Elapsed.TotalSeconds) s"
Start-Sleep -Milliseconds 500
$left = @(Find-Marked $marker)   # a function's empty array unrolls to $null
Check "parent and child are gone" ($left.Count -eq 0) "left: $(($left | ForEach-Object ProcessId) -join ',')"
$left | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }

# 4. A held lock: the timed wait gives up and names the holder.
$lockName = "Global\KhensuBuildLockTest-" + [guid]::NewGuid().ToString("N")
$lib = Join-Path (Split-Path -Parent $PSScriptRoot) "lib\ProcessDeadline.ps1"
$holderScript = ". '$lib'; `$m = Enter-BuildLock -Name '$lockName' -Command 'test holder'; Start-Sleep 20; Exit-BuildLock -Mutex `$m -Name '$lockName'"
$holder = Start-Process -FilePath "pwsh" -ArgumentList @("-NoProfile", "-Command", "`"$holderScript`"") -PassThru -WindowStyle Hidden
try {
    $deadline = (Get-Date).AddSeconds(15)
    while (-not (Test-Path -LiteralPath (Get-BuildLockHolderFile $lockName)) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 200 }
    Check "holder took the lock" (Test-Path -LiteralPath (Get-BuildLockHolderFile $lockName))
    $gaveUp = $false; $message = ""
    try { $m = Enter-BuildLock -TimeoutSec 2 -Name $lockName; Exit-BuildLock -Mutex $m -Name $lockName }
    catch [StepTimeout] { $gaveUp = $true; $message = $_.Exception.Message }
    Check "timed lock wait gives up" $gaveUp
    Check "message names the holder" ($message -match "PID $($holder.Id)" -and $message -match "test holder") $message
} finally {
    Stop-ProcessTree $holder
}

# 5. A lock whose holder died is taken, not waited on.
$m = Enter-BuildLock -TimeoutSec 10 -Name $lockName -Command "after a dead holder"
Check "lock left by a dead holder can be taken" ($null -ne $m)
Exit-BuildLock -Mutex $m -Name $lockName
Check "holder note removed on release" (-not (Test-Path -LiteralPath (Get-BuildLockHolderFile $lockName)))

if ($failures.Count) { Write-Host "test_process_deadline: FAILED ($($failures.Count))" -ForegroundColor Red; exit 1 }
Write-Host "test_process_deadline: OK" -ForegroundColor Green
exit 0
