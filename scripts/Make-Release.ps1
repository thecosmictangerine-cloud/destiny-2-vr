# Packs the player's zip: the built DLL, the installer, the settings template, the README and the
# licences. Build first (build.ps1); this only packs what is in build\x64\Release.
#
#   scripts\Make-Release.ps1 -Version 0.1.0     -> build\dist\Destiny2VR-0.1.0.zip

param(
    [Parameter(Mandatory = $true)][string]$Version
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$package = Join-Path $root 'package'
$dll = Join-Path $root 'build\x64\Release\steam_api64.dll'
$dist = Join-Path $root 'build\dist'
$name = "Destiny2VR-$Version"
$stage = Join-Path $dist $name
$zip = Join-Path $dist "$name.zip"

if (-not (Test-Path $dll)) { throw "No build at ${dll}: run scripts\build.ps1 first." }

# Nothing of this machine goes out inside the DLL: no user name, no source path (a PDB path, a
# __FILE__). build.ps1 strips the PDB path; this catches anything that slips back in.
$latin1 = [System.Text.Encoding]::GetEncoding(28591)
$bytes = $latin1.GetString([System.IO.File]::ReadAllBytes($dll))
$unicode = [System.Text.Encoding]::Unicode
foreach ($needle in @($env:USERNAME, $root)) {
    if (-not $needle) { continue }
    $wide = $latin1.GetString($unicode.GetBytes($needle))
    if ($bytes.IndexOf($needle, [StringComparison]::OrdinalIgnoreCase) -ge 0 -or
        $bytes.IndexOf($wide, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
        throw "The DLL contains '$needle' (a local path or name). Rebuild with scripts\build.ps1."
    }
}

if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
if (Test-Path $zip) { Remove-Item -Force $zip }
New-Item -ItemType Directory -Force -Path $stage | Out-Null

# Text files go out with CRLF: they are opened in Notepad and run by cmd.exe.
function Write-Text([string]$from, [string]$to, [hashtable]$replace = @{}) {
    $text = [System.IO.File]::ReadAllText($from)
    foreach ($key in $replace.Keys) { $text = $text.Replace($key, $replace[$key]) }
    $text = ($text -replace "`r`n", "`n") -replace "`n", "`r`n"
    [System.IO.File]::WriteAllText($to, $text, (New-Object System.Text.UTF8Encoding($false)))
}

Copy-Item $dll (Join-Path $stage 'steam_api64.dll')
Write-Text (Join-Path $package 'README.txt') (Join-Path $stage 'README.txt') @{ '{VERSION}' = $Version }
foreach ($file in 'Install.bat', 'Uninstall.bat', 'Destiny2VR-Setup.ps1', 'SVR_Settings.txt') {
    Write-Text (Join-Path $package $file) (Join-Path $stage $file)
}
Write-Text (Join-Path $root 'LICENSE') (Join-Path $stage 'LICENSE.txt')

# Sunrise's notices, plus the one component the VR module adds to the DLL.
$notices = [System.IO.File]::ReadAllText((Join-Path $root 'THIRD_PARTY_NOTICES.md'))
$notices += @"

## OpenXR headers

The VR module is compiled against the Khronos OpenXR headers (Sunrise/vendor/openxr/include),
unmodified. Copyright 2017-2023 The Khronos Group Inc. SPDX-License-Identifier: Apache-2.0 OR MIT.
"@
$noticesPath = Join-Path $stage 'THIRD-PARTY-NOTICES.txt'
[System.IO.File]::WriteAllText($noticesPath, $notices, (New-Object System.Text.UTF8Encoding($false)))
Write-Text $noticesPath $noticesPath

Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
$hash = (Get-FileHash $zip -Algorithm SHA256).Hash
$dllHash = (Get-FileHash $dll -Algorithm SHA256).Hash
Write-Output "zip     $zip"
Write-Output "  sha256 $hash"
Write-Output "dll     sha256 $dllHash"
Get-ChildItem $stage | ForEach-Object { Write-Output ('  {0,-26} {1,12:N0}' -f $_.Name, $_.Length) }
