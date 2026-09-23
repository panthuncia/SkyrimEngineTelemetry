[CmdletBinding()]
param(
    [string]$Mo2Dir = 'C:\Modding\MO2',
    [string]$Profile = 'Default',
    [string]$ExecutableTitle = 'SKSE',
    [string]$BuildPreset = 'relwithdebinfo-msvc',
    [string]$ConfigurePreset = 'vs2026-msvc',
    [string]$SessionRoot = (Join-Path $PSScriptRoot '..\build\game-runs'),
    [ValidateRange(1, 3600)]
    [int]$ReportIntervalSeconds = 10,
    [switch]$SkipBuild,
    [switch]$DisableSampler,
    [switch]$NoWait
)

$ErrorActionPreference = 'Stop'

$repository = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$mo2Exe = Join-Path $Mo2Dir 'ModOrganizer.exe'
$gameExe = 'C:\Program Files (x86)\Steam\steamapps\common\Skyrim Special Edition\SkyrimSE.exe'
$modRoot = Join-Path $Mo2Dir 'mods\Skyrim Engine Telemetry'
$pluginDir = Join-Path $modRoot 'SKSE\Plugins'
$pluginDll = Join-Path $pluginDir 'SkyrimEngineTelemetry.dll'
$pluginIni = Join-Path $pluginDir 'SkyrimEngineTelemetry.ini'
$profileModList = Join-Path $Mo2Dir "profiles\$Profile\modlist.txt"
$documents = [Environment]::GetFolderPath('MyDocuments')
$skseLogDir = Join-Path $documents 'My Games\Skyrim Special Edition\SKSE'
$reportDir = Join-Path $skseLogDir 'SkyrimEngineTelemetry'

foreach ($required in @($mo2Exe, $gameExe, $profileModList)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Required path was not found: $required"
    }
}
if (Get-Process -Name SkyrimSE -ErrorAction SilentlyContinue) {
    throw 'SkyrimSE is already running. Close it before starting another telemetry session.'
}
if (Get-Process -Name ModOrganizer -ErrorAction SilentlyContinue) {
    throw 'Mod Organizer is already running. Close it so the launcher can select the requested profile reliably.'
}

New-Item -ItemType Directory -Path $modRoot -Force | Out-Null
Set-Content -LiteralPath (Join-Path $repository 'install-prefix.txt') -Value $modRoot -Encoding ASCII

$modList = Get-Content -LiteralPath $profileModList
$entry = '+Skyrim Engine Telemetry'
$existingIndex = [Array]::FindIndex([string[]]$modList, [Predicate[string]] { param($line) $line -match '^[+-]Skyrim Engine Telemetry$' })
if ($existingIndex -ge 0) {
    $modList[$existingIndex] = $entry
} else {
    $modList = @($entry) + $modList
}
Set-Content -LiteralPath $profileModList -Value $modList -Encoding UTF8

if (-not $SkipBuild) {
    & cmake --preset $ConfigurePreset
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed with exit code $LASTEXITCODE" }
    & cmake --build --preset $BuildPreset
    if ($LASTEXITCODE -ne 0) { throw "CMake build failed with exit code $LASTEXITCODE" }
}

foreach ($required in @($pluginDll, $pluginIni)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Post-build deployment did not produce: $required"
    }
}

$ini = Get-Content -Raw -LiteralPath $pluginIni
$samplerValue = if ($DisableSampler) { '0' } else { '1' }
$ini = $ini -replace '(?m)^bEnable\s*=\s*\d+\s*$', "bEnable=$samplerValue"
$ini = $ini -replace '(?m)^iReportIntervalSec\s*=\s*\d+\s*$', "iReportIntervalSec=$ReportIntervalSeconds"
Set-Content -LiteralPath $pluginIni -Value $ini -Encoding ASCII

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$session = Join-Path $SessionRoot $stamp
New-Item -ItemType Directory -Path $session -Force | Out-Null
$dll = Get-Item -LiteralPath $pluginDll
[ordered]@{
    started = (Get-Date).ToString('o')
    repository = $repository
    repository_head = (& git -C $repository rev-parse HEAD)
    mo2 = $mo2Exe
    profile = $Profile
    executable = $ExecutableTitle
    mod_root = $modRoot
    report_directory = $reportDir
    sampler_enabled = -not $DisableSampler
    report_interval_seconds = $ReportIntervalSeconds
    plugin = [ordered]@{
        path = $dll.FullName
        length = $dll.Length
        modified = $dll.LastWriteTime.ToString('o')
        sha256 = (Get-FileHash -LiteralPath $pluginDll -Algorithm SHA256).Hash
    }
} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $session 'session.json') -Encoding UTF8

Write-Host "Telemetry session: $session"
Write-Host "Reports: $reportDir"
Write-Host "Launching MO2 profile '$Profile', executable '$ExecutableTitle'..."
& $mo2Exe -p $Profile run -e $ExecutableTitle
if ($LASTEXITCODE -and $LASTEXITCODE -ne 0) {
    throw "Mod Organizer returned exit code $LASTEXITCODE"
}

$deadline = (Get-Date).AddSeconds(60)
$game = $null
while (-not $game -and (Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 500
    $game = Get-Process -Name SkyrimSE -ErrorAction SilentlyContinue | Select-Object -First 1
}
if (-not $game) { throw 'SkyrimSE did not start within 60 seconds.' }

Set-Content -LiteralPath (Join-Path $session 'pid.txt') -Value $game.Id -Encoding ASCII
Write-Host "SkyrimSE started as PID $($game.Id)."
if (-not $NoWait) {
    Wait-Process -Id $game.Id
    if (Test-Path -LiteralPath $skseLogDir) {
        Get-ChildItem -LiteralPath $skseLogDir -File -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -match '^(SkyrimEngineTelemetry|skse64|CrashLogger|crash-).*\.log\d*$' } |
            Copy-Item -Destination $session -Force
    }
    Set-Content -LiteralPath (Join-Path $session 'completed.txt') -Value (Get-Date).ToString('o') -Encoding ASCII
}

Write-Host "Session artifacts: $session"
