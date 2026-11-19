<#
.SYNOPSIS
  Times the Dashboard's analysis end to end in the real app, with WinINet's
  per-server connection limit set per run (KHENSU_HTTP_CONNECTIONS).

.DESCRIPTION
  For each (file, limit) pair it runs the app's --screenshot-audio check in a
  fresh process and reads analysis-gate-log.txt: when the results gate opened
  ("released after N s") and when the last Dashboard follow-up scan finished
  (the first pendingScans=0 line after release). Use a file nobody analysed
  before for every run - a cached result answers instantly.

.EXAMPLE
  pwsh scripts/time_followup_scans.ps1 -Files a.wav,b.wav,c.wav,d.wav -Limits 2,32,2,32
#>
param(
    [Parameter(Mandatory)] [string[]] $Files,
    [Parameter(Mandatory)] [int[]] $Limits,
    [string] $App = (Join-Path $PSScriptRoot "..\build\desktop-app\KhensuDesktop_artefacts\Release\Khensu Mixing.exe"),
    [string] $OutDir = (Join-Path $PSScriptRoot "..\build\followup-timing")
)
if ($Files.Count -ne $Limits.Count) { throw "one limit per file" }
New-Item -ItemType Directory -Force $OutDir | Out-Null
$rows = @()
for ($i = 0; $i -lt $Files.Count; $i++) {
    $dir = Join-Path $OutDir ("run{0}-limit{1}" -f $i, $Limits[$i])
    Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force $dir | Out-Null
    $env:KHENSU_HTTP_CONNECTIONS = "$($Limits[$i])"
    $p = Start-Process -FilePath $App -ArgumentList "--screenshot=`"$dir`"", "--screenshot-audio=`"$($Files[$i])`"" -PassThru
    if (-not $p.WaitForExit(1800000)) { $p.Kill(); Write-Warning "run $i timed out" }
    Remove-Item Env:KHENSU_HTTP_CONNECTIONS
    $log = Join-Path $dir "analysis-gate-log.txt"
    $released = $null; $scansDone = $null
    if (Test-Path $log) {
        $lines = Get-Content $log
        foreach ($l in $lines) {
            if ($null -eq $released -and $l -match '^released after ([0-9.]+)s') { $released = [double]$Matches[1] }
            if ($null -ne $released -and $null -eq $scansDone -and $l -match '^([0-9.]+)s .*pendingScans=0') {
                $t = [double]$Matches[1]
                if ($t -ge $released) { $scansDone = $t }
            }
        }
    }
    $rows += [pscustomobject]@{ Run = $i; Limit = $Limits[$i]; File = (Split-Path $Files[$i] -Leaf);
                                ResultsReleasedS = $released; FollowUpsDoneS = $scansDone; Exit = $p.ExitCode }
}
$rows | Format-Table -AutoSize
$rows | ConvertTo-Json | Set-Content (Join-Path $OutDir "timing.json")
