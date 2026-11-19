# Keeps one Release build's symbols so its crash minidumps can be read later (SHIP-2).
#
# Release links with /DEBUG (cmake/CompilerFlags.cmake), so each binary has a
# .pdb beside it. The installer leaves the PDBs out; this copies every shipped
# binary and its PDB into build/symbols/<version>-<commit>/, the folder to
# point a debugger's symbol path (or `cdb -y`) at for a dump from that build.
#
#   pwsh scripts/archive_symbols.ps1 [-BuildDir build] [-Dest build/symbols]
param(
    [string]$BuildDir = "build",
    [string]$Dest = ""
)
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo
if (-not $Dest) { $Dest = Join-Path $BuildDir "symbols" }

$versionHeader = Get-Content "shared-core/include/khensu/common/Version.h" -Raw
$version = if ($versionHeader -match '"(\d+\.\d+\.\d+[^"]*)"') { $Matches[1] } else { "unknown" }
$commit = (git rev-parse --short=10 HEAD).Trim()
$dirty = if (git status --porcelain --untracked-files=no) { "-dirty" } else { "" }
$target = Join-Path $Dest "$version-$commit$dirty"

$binaries = @(
    "$BuildDir/desktop-app/KhensuDesktop_artefacts/Release/Khensu Mixing.exe",
    "$BuildDir/plugin-copilot/KhensuCopilot_artefacts/Release/VST3/Khensu Copilot.vst3/Contents/x86_64-win/Khensu Copilot.vst3"
)
$copied = 0
New-Item -ItemType Directory -Force $target | Out-Null
foreach ($bin in $binaries) {
    if (-not (Test-Path -LiteralPath $bin)) { Write-Warning "missing: $bin"; continue }
    $pdb = [IO.Path]::ChangeExtension($bin, ".pdb")
    if (-not (Test-Path -LiteralPath $pdb)) {
        # A plugin's PDB sits beside its bundle (VST3/Khensu Copilot.pdb), not
        # inside it: walk up to three folders looking for <name>.pdb.
        $name = [IO.Path]::GetFileNameWithoutExtension($bin) + ".pdb"
        $dir = Split-Path $bin
        $pdb = $null
        for ($i = 0; $i -lt 4 -and $dir -and -not $pdb; $i++) {
            $candidate = Join-Path $dir $name
            if (Test-Path -LiteralPath $candidate) { $pdb = $candidate }
            $dir = Split-Path $dir
        }
    }
    if (-not $pdb) { Write-Warning "no PDB beside $bin (configure with this tree's CompilerFlags.cmake)"; continue }
    Copy-Item -LiteralPath $bin, $pdb -Destination $target -Force
    $copied++
}
if ($copied -eq 0) { Write-Error "no binaries with symbols found under $BuildDir"; exit 1 }
"symbols for $copied binaries -> $target"
