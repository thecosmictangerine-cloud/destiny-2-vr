# The barrel shot as a person would judge it, in a running session (a first-person area, F9 on).
#
# Test-Aim.ps1 measures with the gun out of frame; this is the picture with the gun IN frame: for each
# pose, the drawn aim line on (`ray on`, magenta), one round, and captures at the moments that matter
# -- the muzzle flash and tracer (tens of milliseconds) and the impact (about a tenth of a second) --
# laid out on one sheet. What to look for:
#
#   flash      at the mouth of the drawn barrel, not at the eye or the screen centre
#   tracer     leaving the barrel along the magenta line
#   impact     at the end of that line
#   side       the gun across the view, so the line can be seen leaving the barrel's mouth (muzzle
#              calibration: `muzzle <fwd> <right> <up>` in SVR_Aim.txt)
param(
    [string]$Out = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'build\shots\aimshow'),
    [int[]]$At = @(35, 70, 120),
    [int]$HoldMs = 60,
    [switch]$NoRay
)
$ErrorActionPreference = 'Stop'
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'lib\GameIO.ps1')
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'lib\VrCommands.ps1')
if (-not (Test-Path $Out)) { New-Item -ItemType Directory -Force -Path $Out | Out-Null }

$poses = [ordered]@{
    front = @{ HandX = 0.05; HandY = 0.12; HandZ = -0.20; HandYaw = -8; HandPitch = -6 }
    left  = @{ HandX = 0.05; HandY = 0.12; HandZ = -0.20; HandYaw = 14; HandPitch = -4 }
    side  = @{ HandX = 0.15; HandY = 0.10; HandZ = -0.35; HandYaw = 70; HandPitch = -3 }
}

[void](Focus-Game)
[void](Send-Aim @($(if ($NoRay) { 'ray off' } else { 'ray on' }), 'shot aim'))
$frames = New-Object System.Collections.ArrayList
foreach ($name in $poses.Keys) {
    $p = $poses[$name]
    [void](Set-MockInput @p)
    Start-Sleep -Seconds 1.6
    Clear-WeaponIdle -SettleMs 900 | Out-Null
    $before = Join-Path $Out "${name}_0.png"
    Save-Shot $before | Out-Null
    [void]$frames.Add($before)
    $t0 = Get-Date
    [SunriseVR.Native]::mouse_event(0x0002, 0, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds $HoldMs
    [SunriseVR.Native]::mouse_event(0x0004, 0, 0, 0, [IntPtr]::Zero)
    foreach ($t in $At) {
        $wait = $t - ((Get-Date) - $t0).TotalMilliseconds
        if ($wait -gt 0) { Start-Sleep -Milliseconds $wait }
        $f = Join-Path $Out "${name}_$t.png"
        Save-Shot $f | Out-Null
        [void]$frames.Add($f)
    }
    Start-Sleep -Milliseconds 800
    Write-Host ("{0,-6} {1}" -f $name, ((Get-VrLog 'ev=vr\.aim shot ' 1) -replace '.*(serial=\d+ points=\d+ rewritten=\d).*(turned_deg=[-\d.]+).*', '$1 $2'))
}
$cols = 1 + $At.Count
$sheet = Join-Path $Out 'sheet.png'
& python -c @"
import sys
from PIL import Image, ImageDraw
files = sys.argv[2:]
cols = int(sys.argv[1])
w, h = 640, 360
rows = (len(files) + cols - 1) // cols
S = Image.new('RGB', (w * cols, h * rows))
for i, f in enumerate(files):
    im = Image.open(f).convert('RGB').resize((w, h))
    ImageDraw.Draw(im).text((8, 8), f.split('\\')[-1], fill=(255, 255, 0))
    S.paste(im, ((i % cols) * w, (i // cols) * h))
S.save(r'$sheet')
"@ $cols @frames
Write-Host "sheet: $sheet"
$rest = @{ HandX = 0.05; HandY = 0.12; HandZ = -0.20 }
[void](Set-MockInput @rest)
