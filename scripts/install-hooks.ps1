# scripts/install-hooks.ps1
# Points git at the versioned hooks in .githooks/.
#
# Git hooks live in .git/hooks, which is NOT versioned and NOT copied on clone,
# so a hook committed to the repo does nothing until git is told where to look.
# core.hooksPath fixes that in one setting, and it is per-clone — every
# contributor runs this once.
#
#   pwsh scripts/install-hooks.ps1
#
# See .githooks/pre-push for what the hooks enforce and why they exist
# (audit CFG-01: branch protection is not available on every GitHub plan).

$root = Split-Path -Parent $PSScriptRoot
Push-Location $root
try {
    git config core.hooksPath .githooks
    $current = git config --get core.hooksPath
    if ($current -eq '.githooks') {
        Write-Host "core.hooksPath -> .githooks"
        Write-Host "pre-push will now run the doc-drift and lint gates before every push."
    } else {
        Write-Error "Failed to set core.hooksPath (got '$current')."
        exit 1
    }
} finally {
    Pop-Location
}
