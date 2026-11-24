<#
.SYNOPSIS
  Installs (or removes) the self-hosted GitHub Actions runner that runs
  .github/workflows/self-hosted.yml on this PC (OVH-3).

.DESCRIPTION
  * Downloads the official actions/runner release for win-x64 and refuses it
    unless its SHA-256 matches the digest GitHub publishes for the asset and
    the checksum in the release notes.
  * Registers it to the PRIVATE repository only (KhensuLabs/Khensunian-Mixing)
    with the label `khensu-local`, using a short-lived registration token from
    `gh` (the caller needs admin on the repository). Never register it to the
    public snapshot: anyone can open a pull request there.
  * Starts it now, and at every logon through a per-user scheduled task
    ("Khensu CI runner"), so no administrator rights or Windows service are
    needed. Jobs run as the logged-in user.
  * Keeps all CI state under D:\khensu-ci (runner, work folder, build tree,
    FetchContent cache, venv, logs) - outside the Drive-synced project folder.

  -Remove unregisters the runner from GitHub, deletes the scheduled task,
  stops the runner and deletes D:\khensu-ci\runner (the build cache and logs
  are kept unless -Purge).

.EXAMPLE
  pwsh scripts/setup_self_hosted_runner.ps1
.EXAMPLE
  pwsh scripts/setup_self_hosted_runner.ps1 -Remove -Purge
#>
param(
    [string]$Repo = "KhensuLabs/Khensunian-Mixing",
    [string]$CiRoot = "D:\khensu-ci",
    [string]$RunnerName = "$env:COMPUTERNAME-khensu",
    [switch]$Remove,
    [switch]$Purge
)
$ErrorActionPreference = "Stop"
if ($Repo -match '/khensu-mixing$') { throw "refusing: never attach a self-hosted runner to the public repository" }
$runnerDir = Join-Path $CiRoot "runner"
$taskName = "Khensu CI runner"

function Stop-Runner {
    Get-CimInstance Win32_Process -Filter "Name = 'Runner.Listener.exe' OR Name = 'Runner.Worker.exe'" |
        Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($runnerDir, [StringComparison]::OrdinalIgnoreCase) } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
}

if ($Remove) {
    Unregister-ScheduledTask -TaskName $taskName -Confirm:$false -ErrorAction SilentlyContinue
    Stop-Runner
    if (Test-Path (Join-Path $runnerDir "config.cmd")) {
        $token = gh api -X POST "repos/$Repo/actions/runners/remove-token" --jq .token
        & (Join-Path $runnerDir "config.cmd") remove --token $token
    }
    if (Test-Path $runnerDir) { Remove-Item $runnerDir -Recurse -Force }
    if ($Purge -and (Test-Path $CiRoot)) { Remove-Item $CiRoot -Recurse -Force }
    Write-Host "Runner removed."
    return
}

New-Item -ItemType Directory -Force -Path $runnerDir, (Join-Path $CiRoot "work") | Out-Null

if (-not (Test-Path (Join-Path $runnerDir "config.cmd"))) {
    $release = gh api repos/actions/runner/releases/latest | ConvertFrom-Json
    $asset = $release.assets | Where-Object { $_.name -match '^actions-runner-win-x64-[\d.]+\.zip$' } | Select-Object -First 1
    if (-not $asset) { throw "no win-x64 asset in actions/runner $($release.tag_name)" }
    $notesSha = [regex]::Match($release.body, '<!-- BEGIN SHA win-x64 -->([0-9a-f]{64})<!-- END SHA win-x64 -->').Groups[1].Value
    $digestSha = "$($asset.digest)" -replace '^sha256:', ''
    if (-not $notesSha -or $notesSha -ne $digestSha) { throw "runner checksum: release notes '$notesSha' vs asset digest '$digestSha'" }
    $zip = Join-Path $CiRoot $asset.name
    Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $zip
    $actual = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $notesSha) { Remove-Item $zip; throw "downloaded runner SHA-256 $actual does not match $notesSha" }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [System.IO.Compression.ZipFile]::ExtractToDirectory($zip, $runnerDir)
    Remove-Item $zip
    Write-Host "Runner $($release.tag_name) extracted (SHA-256 verified)."
}

if (-not (Test-Path (Join-Path $runnerDir ".runner"))) {
    $token = gh api -X POST "repos/$Repo/actions/runners/registration-token" --jq .token
    if (-not $token) { throw "could not get a registration token (admin on $Repo needed)" }
    & (Join-Path $runnerDir "config.cmd") --unattended --url "https://github.com/$Repo" --token $token `
        --name $RunnerName --labels khensu-local --work (Join-Path $CiRoot "work") --replace
    if ($LASTEXITCODE -ne 0) { throw "runner configuration failed" }
}

# Start at logon (per user, no admin) and now, and re-check every 5 minutes:
# the first CI run (2026-11-23) lost its runner mid-job - the listener was
# shut down and nothing started it again. With MultipleInstances IgnoreNew
# a running runner is left alone; a stopped one comes back within 5 min.
$action = New-ScheduledTaskAction -Execute (Join-Path $runnerDir "run.cmd") -WorkingDirectory $runnerDir
$trigger = @(
    (New-ScheduledTaskTrigger -AtLogOn -User "$env:USERDOMAIN\$env:USERNAME"),
    (New-ScheduledTaskTrigger -Once -At (Get-Date).AddMinutes(1) -RepetitionInterval (New-TimeSpan -Minutes 5))
)
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
    -ExecutionTimeLimit ([TimeSpan]::Zero) -MultipleInstances IgnoreNew
Register-ScheduledTask -TaskName $taskName -Action $action -Trigger $trigger -Settings $settings `
    -Description "GitHub Actions runner for $Repo (scripts/setup_self_hosted_runner.ps1)" -Force | Out-Null
Stop-Runner
Start-ScheduledTask -TaskName $taskName
Write-Host "Runner '$RunnerName' registered to $Repo (label khensu-local) and started; it starts at every logon."
