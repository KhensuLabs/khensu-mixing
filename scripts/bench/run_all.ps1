# scripts/bench/run_all.ps1
# Runs every performance benchmark and writes results under bench-results/.
#   pwsh scripts/bench/run_all.ps1            # full matrix (~10-15 min)
#   pwsh scripts/bench/run_all.ps1 -Quick     # 30 s / 3 min files only
#   pwsh scripts/bench/run_all.ps1 -SkipApp   # no whole-app --ui-bench pass
# Needs a Release build (build/) and the service venv (local-services/.venv).
# Exit code: 0 = everything within budget, 1 = something over budget or failed.
# Budgets and the 2026-10-17 baseline: docs/archive/2026-10-17-performance-baseline.md.
param(
    [switch]$Quick,
    [switch]$SkipApp,
    [string]$OutDir = ""
)
$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
if (-not $OutDir) { $OutDir = Join-Path $repo "bench-results" }
New-Item -ItemType Directory -Force $OutDir | Out-Null

$python = Join-Path $repo "local-services\.venv\Scripts\python.exe"
$coreTests = Join-Path $repo "build\tests\Release\KhensuCoreTests.exe"
$uiTests = Join-Path $repo "build\tests\Release\KhensuTests.exe"
$app = Join-Path $repo "build\desktop-app\KhensuDesktop_artefacts\Release\Khensu Mixing.exe"
$failed = @()

function Step($name, [scriptblock]$body) {
    Write-Host "== $name"
    $sw = [Diagnostics.Stopwatch]::StartNew()
    & $body
    $code = $LASTEXITCODE
    Write-Host ("   {0} (exit {1}, {2:N0} s)" -f $(if ($code -eq 0) { "ok" } else { "OVER BUDGET / FAILED" }), $code, $sw.Elapsed.TotalSeconds)
    if ($code -ne 0) { $script:failed += $name }
}

foreach ($exe in @($python, $coreTests, $uiTests)) {
    if (-not (Test-Path $exe)) { Write-Error "missing $exe - build Release / create the venv first" }
}

Step "Python intake (time, peak memory, long-session RSS)" {
    $args = @("-W", "ignore", (Join-Path $repo "scripts\bench\intake_bench.py"), "--out", (Join-Path $OutDir "intake.json"))
    if ($Quick) { $args += "--quick" }
    & $python @args
}
Step "C++ AudioAnalyzer throughput" {
    & $coreTests "[.bench]" --reporter console --out (Join-Path $OutDir "core-bench.txt")
}
Step "C++ shared widget paint / layout" {
    & $uiTests "[.bench]" --reporter console --out (Join-Path $OutDir "ui-widget-bench.txt")
}
if (-not $SkipApp) {
    if (Test-Path $app) {
        Step "Whole-app screens, Dashboard update, session-switch memory (--ui-bench)" {
            $report = Join-Path $OutDir "ui-bench.json"
            $p = Start-Process -FilePath $app -ArgumentList "--ui-bench=`"$report`"" -PassThru -Wait
            $global:LASTEXITCODE = $p.ExitCode
            if (Test-Path $report) { Get-Content $report | Write-Host }
        }
    } else {
        Write-Host "== skipping --ui-bench: $app not built"
    }
}

Write-Host ""
if ($failed.Count -eq 0) {
    Write-Host "All benchmarks within budget. Results: $OutDir"
    exit 0
}
Write-Host ("Over budget / failed: " + ($failed -join "; "))
exit 1
