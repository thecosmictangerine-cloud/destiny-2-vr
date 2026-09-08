# Builds the mod DLL.
#
# Two overrides are needed on this machine and neither is a project change:
#   PlatformToolset=v143         the project asks for v145 (VS 2026); only VS 2022 is installed
#   PreferredToolArchitecture=x64  the 32-bit compiler runs out of heap (C1060) on this codebase
#
# Parallelism is capped because 16 GB has to hold four compilers and, often, the running game.

param(
    [string]$Configuration = 'Release',
    [int]$MaxCpu = 4,
    [switch]$Rebuild
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$sln = Join-Path $root 'Sunrise.sln'
$msbuild = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe'
$logDir = Join-Path $root 'build\logs'
if (-not (Test-Path $logDir)) { New-Item -ItemType Directory -Force -Path $logDir | Out-Null }
$log = Join-Path $logDir ('build-{0}.log' -f (Get-Date -Format 'yyyyMMdd-HHmmss'))

if (-not (Test-Path $msbuild)) { throw "MSBuild not found at $msbuild" }

# A build cannot replace a DLL the game has mapped.
Get-Process -Name destiny2 -ErrorAction SilentlyContinue | Stop-Process -Force

$target = if ($Rebuild) { '/t:Rebuild' } else { '/t:Build' }

& $msbuild $sln $target "/m:$MaxCpu" "/p:Configuration=$Configuration" /p:Platform=x64 `
    /p:PlatformToolset=v143 /p:PreferredToolArchitecture=x64 `
    /verbosity:minimal /nologo /fl "/flp:logfile=$log;verbosity=normal"

$code = $LASTEXITCODE
$dll = Join-Path $root ('build\x64\{0}\steam_api64.dll' -f $Configuration)

if ($code -ne 0) {
    Write-Output "BUILD FAILED (exit $code). Log: $log"
    Select-String -Path $log -Pattern 'error (C|LNK|MSB)[0-9]+' |
        Select-Object -First 15 |
        ForEach-Object { $_.Line.Trim() }
    exit $code
}

Write-Output ('BUILD OK -> {0}' -f $dll)
Write-Output ('  sha256 {0}' -f (Get-FileHash $dll -Algorithm SHA256).Hash)
Write-Output ('  log    {0}' -f $log)
