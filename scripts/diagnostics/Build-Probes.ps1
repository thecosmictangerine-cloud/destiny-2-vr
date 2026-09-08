$ErrorActionPreference = 'Stop'
$vcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$src = Join-Path $repo 'mockxr\probe_dll.cpp'
$out = Join-Path $repo 'build\mockxr'
$noCrt = 'cl /nologo /LD /O2 /GS- /DSVR_PROBE_NOCRT ' +
         ('/Fo"{0}\\probe_nocrt.obj" /Fe"{0}\SVR_Probe_NoCrt.dll" "{1}" ' -f $out, $src) +
         '/link /NODEFAULTLIB /ENTRY:DllMain kernel32.lib'
$crt = 'cl /nologo /LD /O2 /MT ' +
       ('/Fo"{0}\\probe_crt.obj" /Fe"{0}\SVR_Probe_Crt.dll" "{1}" ' -f $out, $src) +
       '/link kernel32.lib'
& cmd /c "`"$vcvars`" >nul 2>nul && $noCrt"
if ($LASTEXITCODE -ne 0) { throw "nocrt probe build failed" }
& cmd /c "`"$vcvars`" >nul 2>nul && $crt"
if ($LASTEXITCODE -ne 0) { throw "crt probe build failed" }
Copy-Item "$out\SVR_Probe_NoCrt.dll" C:\Games\Sunrise\ -Force
Copy-Item "$out\SVR_Probe_Crt.dll" C:\Games\Sunrise\ -Force
Remove-Item C:\Games\Sunrise\SVR_Probe_NoCrt.txt, C:\Games\Sunrise\SVR_Probe_Crt.txt -ErrorAction SilentlyContinue
Get-Item "$out\SVR_Probe_*.dll" | Select-Object Name, Length
