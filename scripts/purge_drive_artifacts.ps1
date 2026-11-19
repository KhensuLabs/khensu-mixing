# scripts/purge_drive_artifacts.ps1
# Removes Google Drive's desktop.ini artifacts from build/venv trees.
#
# Why (audit 2026-07-16, finding F10): this repo lives in a Drive-synced
# folder, and Drive writes desktop.ini into every directory it touches -
# including .venv/Lib/site-packages, where jsonschema_specifications
# crashes iterating its schemas dir (NotADirectoryError), failing four
# pytest cases that are green on CI. Drive also holds file locks that can
# break MSBuild (.tlog/.obj permission errors).
#
# Run whenever those symptoms reappear:
#   pwsh scripts/purge_drive_artifacts.ps1
#
# Durable fix: exclude .venv/, build/, .pytest_tmp/ from Drive sync (or
# move the working repo out of the Drive folder and let git do the sync).

$root = Split-Path -Parent $PSScriptRoot
$targets = @(
    (Join-Path $root "local-services\.venv"),
    (Join-Path $root "local-services\.venv311"),
    (Join-Path $root "local-services\.pytest_tmp"),
    (Join-Path $root "build")
)

$total = 0
foreach ($dir in $targets) {
    if (-not (Test-Path $dir)) { continue }
    $files = Get-ChildItem -Path $dir -Filter "desktop.ini" -Recurse -Force -File -ErrorAction SilentlyContinue
    $count = ($files | Measure-Object).Count
    if ($count -gt 0) {
        $files | Remove-Item -Force -ErrorAction SilentlyContinue
        Write-Host "Removed $count desktop.ini under $dir"
    }
    $total += $count
}
Write-Host "Done - $total desktop.ini file(s) removed."
