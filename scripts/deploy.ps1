# Puts the built DLL into the game, or puts the stock one back.
#
# Sunrise loads by standing in for the game's own steam_api64.dll, so deploying is a file swap.
# The stock 0.3.2 DLL is kept beside this script's output the first time it is displaced, and the
# installer's copy of the untouched Steam DLL stays in <game>\.sunrise\original either way.

param(
    [string]$Configuration = 'Release',
    [switch]$Restore
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$gameDir = 'C:\Games\Sunrise'
$target = Join-Path $gameDir 'bin\x64\steam_api64.dll'
$backupDir = Join-Path $root 'build\stock'
$backup = Join-Path $backupDir 'steam_api64.stock.dll'

Get-Process -Name destiny2 -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 2

if ($Restore) {
    if (-not (Test-Path $backup)) { throw "no stock DLL kept at $backup" }
    Copy-Item $backup $target -Force
    Write-Output ('RESTORED stock DLL -> {0}' -f $target)
    Write-Output ('  sha256 {0}' -f (Get-FileHash $target -Algorithm SHA256).Hash)
    exit 0
}

$dll = Join-Path $root ('build\x64\{0}\steam_api64.dll' -f $Configuration)
if (-not (Test-Path $dll)) { throw "nothing built at $dll" }

if (-not (Test-Path $backup)) {
    if (-not (Test-Path $backupDir)) { New-Item -ItemType Directory -Force -Path $backupDir | Out-Null }
    Copy-Item $target $backup -Force
    Write-Output ('kept stock DLL -> {0}' -f $backup)
}

Copy-Item $dll $target -Force
Write-Output ('DEPLOYED -> {0}' -f $target)
Write-Output ('  sha256 {0}' -f (Get-FileHash $target -Algorithm SHA256).Hash)
