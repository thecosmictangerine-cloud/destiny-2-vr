# Builds and runs the mock runtime's standalone test (mock_harness.cpp) against the mock DLL.
#
# No game, no headset: the harness loads the DLL as a loader does, uses a WARP D3D11 device, and
# runs in its own scratch directory (the mock reads and writes its SVR_* files in the working
# directory), so nothing in C:\Games\Sunrise is touched. Exit code = number of failed checks.
#
#   .\Run-MockHarness.ps1                      # builds the harness, tests build\mockxr\SunriseVR_MockXR.dll
#   .\Run-MockHarness.ps1 -WorkDir D:\tmp\mx   # somewhere else for the composed images and the log
#
# Build the mock first (scripts\build-mockxr.ps1). The composed eye images land in the work
# directory as SVR_seen_<tag>_eye<i>.bmp: hq (projection + 5 quads), qonly (quads, no projection),
# seqq_f00..f02 (a sequence), recentred (after a recentre).
param(
    [string]$Dll = '',
    [string]$WorkDir = ''
)
$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$out = Join-Path $repo 'build\mockxr\tests'
if (-not $Dll) { $Dll = Join-Path $repo 'build\mockxr\SunriseVR_MockXR.dll' }
if (-not $WorkDir) { $WorkDir = Join-Path $out 'run' }
if (-not (Test-Path $Dll)) { throw "mock DLL not found: $Dll (run scripts\build-mockxr.ps1)" }
foreach ($d in @($out, $WorkDir)) { if (-not (Test-Path $d)) { New-Item -ItemType Directory -Force -Path $d | Out-Null } }

$vcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
$src = Join-Path $PSScriptRoot 'mock_harness.cpp'
$inc = Join-Path $repo 'Sunrise\vendor\openxr\include'
$exe = Join-Path $out 'mock_harness.exe'
$cl = 'cl /nologo /std:c++17 /MT /O2 /EHsc /W3 ' +
      ('/I"{0}" ' -f $inc) +
      ('/Fo"{0}\\" /Fd"{0}\\" ' -f $out) +
      ('/Fe"{0}" "{1}" ' -f $exe, $src) +
      '/link d3d11.lib'
& cmd /c "`"$vcvars`" >nul 2>nul && $cl"
if ($LASTEXITCODE -ne 0) { throw "harness build failed (exit $LASTEXITCODE)" }

# A clean directory: old composed images or a stale event file would read as results.
Get-ChildItem -Path $WorkDir -Filter 'SVR_*' -ErrorAction SilentlyContinue | Remove-Item -Force
Push-Location $WorkDir
try {
    & $exe $Dll
    $code = $LASTEXITCODE
} finally {
    Pop-Location
}
Write-Output ('harness exit {0}; images and SVR_MockXR.log in {1}' -f $code, $WorkDir)
exit $code
