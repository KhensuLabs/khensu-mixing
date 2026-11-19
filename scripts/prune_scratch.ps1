# Prunes the scratch that accumulates inside the working copy (CLN-4).
#
# All of it is git-ignored and inside the Drive-synced tree, so nothing else
# reminds you it is there:
#   - pytest basetemps: local-services/.pytest_tmp* and .pytest_tmp* at the
#     root. Each concurrent run needs its own --basetemp, so they multiply;
#     any that has not been written for -Days is removed. Inside the shared
#     .pytest_tmp only the stale entries go (run_python_tests.ps1 does the
#     same before each run).
#   - build/verify-logs: verify_all.ps1 writes one folder per run; the newest
#     -KeepLogs stay.
#   - .git/objects/??/tmp_obj_*: half-written objects left by an interrupted
#     git process (`git count-objects -v` reports them as garbage). Only ones
#     older than a day, so a running git is never raced.
#
#   pwsh scripts/prune_scratch.ps1 [-Days 2] [-KeepLogs 5] [-BuildDir build] [-WhatIf]
[CmdletBinding(SupportsShouldProcess)]
param(
    [int]$Days = 2,
    [int]$KeepLogs = 5,
    [string]$BuildDir = "build"
)
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$cutoff = (Get-Date).AddDays(-$Days)
$removed = 0

function Remove-Scratch([System.IO.FileSystemInfo]$item) {
    if ($PSCmdlet.ShouldProcess($item.FullName, "remove")) {
        # Drive holds locks: skip a locked file rather than abort; the next
        # run retries it.
        Remove-Item -LiteralPath $item.FullName -Recurse -Force -Confirm:$false -ErrorAction SilentlyContinue
        if (-not (Test-Path -LiteralPath $item.FullName)) { $script:removed++ }
    }
}

foreach ($root in @($repo, (Join-Path $repo "local-services"))) {
    foreach ($dir in Get-ChildItem -LiteralPath $root -Directory -Force -Filter ".pytest_tmp*" -ErrorAction SilentlyContinue) {
        if ($dir.Name -eq ".pytest_tmp") {
            Get-ChildItem -LiteralPath $dir.FullName -Force |
                Where-Object { $_.LastWriteTime -lt $cutoff } |
                ForEach-Object { Remove-Scratch $_ }
        } elseif ($dir.LastWriteTime -lt $cutoff) {
            Remove-Scratch $dir
        }
    }
}

$buildRoot = if ([IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $repo $BuildDir }
$logs = Join-Path $buildRoot "verify-logs"
if (Test-Path -LiteralPath $logs) {
    Get-ChildItem -LiteralPath $logs -Directory |
        Sort-Object LastWriteTime -Descending |
        Select-Object -Skip ([Math]::Max(0, $KeepLogs)) |
        ForEach-Object { Remove-Scratch $_ }
}

$objects = Join-Path $repo ".git/objects"
if (Test-Path -LiteralPath $objects) {
    Get-ChildItem -LiteralPath $objects -Directory -Filter "??" |
        ForEach-Object { Get-ChildItem -LiteralPath $_.FullName -File -Filter "tmp_obj_*" } |
        Where-Object { $_.LastWriteTime -lt (Get-Date).AddDays(-1) } |
        ForEach-Object { Remove-Scratch $_ }
}

"pruned $removed item(s)"
