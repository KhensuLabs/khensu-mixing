# Run the local-services pytest suite with the repository's supported
# interpreter and Windows temp-directory isolation.
#
#   pwsh scripts/run_python_tests.ps1 -Quick
#   pwsh scripts/run_python_tests.ps1

param(
    [switch]$Quick
)

$root = Split-Path -Parent $PSScriptRoot
$serviceRoot = Join-Path $root "local-services"
$python = Join-Path $serviceRoot ".venv\Scripts\python.exe"
$quickTests = @(
    "tests/test_analysis_request_limits.py",
    "tests/test_analyze_models.py",
    "tests/test_channel_layout.py",
    "tests/test_credential_protector.py",
    "tests/test_db_unify.py",
    "tests/test_evidence_graph.py",
    "tests/test_genre_schema.py",
    "tests/test_integrity_checker.py",
    "tests/test_key.py",
    "tests/test_model_manifest.py",
    "tests/test_online_gate.py",
    "tests/test_quick_stats.py",
    "tests/test_render_finite_guards.py",
    "tests/test_render_output_safety.py",
    "tests/test_render_validator.py",
    "tests/test_server_health.py",
    "tests/test_surround_validation.py",
    "tests/test_training_provenance.py"
)

if (-not (Test-Path -LiteralPath $python)) {
    throw "Repository interpreter not found at '$python'. Create local-services/.venv and install the package first."
}

# Prune stale .pytest_tmp content BEFORE the run (audit AUD-05). pytest's own
# numbered-root rotation only keeps 3 roots, but roots left by crashed runs
# grow unbounded - this dir reached 620 MB twice. Deleting up front (rather
# than after) also means a green run leaves only its own fresh output behind.
# Google Drive file locks can make individual deletes fail; -ErrorAction
# SilentlyContinue skips the locked file rather than aborting the test run,
# and the next invocation retries it.
$cutoff = (Get-Date).AddDays(-2)
$pytestTmp = Join-Path $serviceRoot ".pytest_tmp"
if (Test-Path -LiteralPath $pytestTmp) {
    Get-ChildItem -LiteralPath $pytestTmp -Force |
        Where-Object { $_.LastWriteTime -lt $cutoff } |
        Remove-Item -Recurse -Force -Confirm:$false -ErrorAction SilentlyContinue
}
# PKG-4: concurrent runs each take their own basetemp (.pytest_tmp_<name>, see
# CLAUDE.md), and nothing pruned those - they reached 2 GB. A whole folder goes
# once nothing in it has been written for two days, so a run in progress (whose
# folder it is) is never touched.
Get-ChildItem -LiteralPath $serviceRoot -Directory -Force -Filter ".pytest_tmp_*" -ErrorAction SilentlyContinue |
    Where-Object {
        $newest = Get-ChildItem -LiteralPath $_.FullName -Recurse -Force -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1
        $stamp = if ($null -ne $newest) { $newest.LastWriteTime } else { $_.LastWriteTime }
        $stamp -lt $cutoff
    } |
    Remove-Item -Recurse -Force -Confirm:$false -ErrorAction SilentlyContinue

Push-Location $serviceRoot
try {
    if ($Quick) {
        & $python -m pytest -q --basetemp=.pytest_tmp @quickTests
    } else {
        & $python -m pytest -q --basetemp=.pytest_tmp
    }
    exit $LASTEXITCODE
} finally {
    Pop-Location
}
