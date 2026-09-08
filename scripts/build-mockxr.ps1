# Builds the mock OpenXR runtime and writes its loader manifest.
#
# The mock IS a runtime, so it must never link the OpenXR loader: headers and d3d11 only. That
# also means no vcpkg and no CMake are needed — one cl.exe call does it.
#
# Point a game run at it with:
#   $env:XR_RUNTIME_JSON = '<printed manifest path>'
#
# It is driven by two text files read from the GAME's working directory (C:\Games\Sunrise):
#   SVR_MockHmd.txt    "w h fovL fovR fovU fovD"  — per-eye size, then LEFT-eye FOV in degrees
#                      (right eye mirrored). Quest 3: "1824 1968 -52 42 48 -50"
#   SVR_MockInput.txt  synthetic poses, whitespace separated, in this order:
#                      turnX trigR handYaw handPitch headYaw headPitch headX headY headZ
#                      moveX moveY handX handY handZ btnA btnB btnX btnY turnY trigL
#                      gripL gripR thumbL thumbR menu ...
#                      Fields left out KEEP THEIR PREVIOUS VALUE. headY is an offset on a 1.6 m
#                      standing height, not the height itself.
# It writes SVR_MockXR.log beside those files.

$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
$out = Join-Path $repo 'build\mockxr'
$vcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found at $vcvars" }
if (-not (Test-Path $out)) { New-Item -ItemType Directory -Force -Path $out | Out-Null }

$src = Join-Path $repo 'mockxr\mock_runtime.cpp'
$inc = Join-Path $repo 'Sunrise\vendor\openxr\include'
$dll = Join-Path $out 'SunriseVR_MockXR.dll'

# Static CRT on purpose. With /MD the DLL failed to load into the game with ERROR_DLL_INIT_FAILED
# (1114): it gets loaded from outside the game directory, and the VC runtimes the game ships are
# from 2020, too old for a VS 2022 build. /MT makes the mock depend on nothing.
$cl = 'cl /nologo /LD /std:c++17 /MT /O2 /EHsc /W3 ' +
      ('/I"{0}" ' -f $inc) +
      ('/Fo"{0}\\" /Fd"{0}\\" ' -f $out) +
      ('/Fe"{0}" "{1}" ' -f $dll, $src) +
      '/link d3d11.lib'

# vcvars prints a vswhere warning to stderr on this machine. Silence it INSIDE cmd: capturing a
# native command's stderr in PowerShell 5.1 turns each line into an error record and fails the
# script even when the exit code is zero. The redirect applies only to vcvars, not to cl.
& cmd /c "`"$vcvars`" >nul 2>nul && $cl"
if ($LASTEXITCODE -ne 0) { throw "mock build failed (exit $LASTEXITCODE)" }
if (-not (Test-Path $dll)) { throw "mock build produced no DLL" }

# The manifest is what the loader reads; library_path needs its backslashes escaped for JSON.
$escaped = $dll -replace '\\', '\\'
$manifest = Join-Path $out 'SunriseVR_MockXR.json'
$json = @"
{
  "file_format_version": "1.0.0",
  "runtime": {
    "library_path": "$escaped"
  }
}
"@
[System.IO.File]::WriteAllText($manifest, $json, (New-Object System.Text.UTF8Encoding($false)))

Write-Output ('MOCK OK   -> {0}' -f $dll)
Write-Output ('  manifest {0}' -f $manifest)
Write-Output ('  activate $env:XR_RUNTIME_JSON = ''{0}''' -f $manifest)
