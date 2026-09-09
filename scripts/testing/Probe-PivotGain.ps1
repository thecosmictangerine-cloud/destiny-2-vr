# Does the pivot term actually reach the screen, and at full strength?
#
# Asked because the calibration measured a response about ten times too small. Under a 15 degree
# hand yaw the weapon moved 197 px, which matches |d| = 0.33 m almost exactly (2*0.33*sin(7.5) =
# 0.086 m, and 0.08 m is a measured 166 px). So `d` is real and the baseline is right. But probing
# the pivot by 0.08 m forward changed that residual by 5 px where the geometry demands about 43.
#
# Two candidate explanations, and this separates them in one run:
#
#   the module writes the wrong lanes   -> the `out=` values in the transform dump will not move by
#                                          the pivot when the pivot changes
#   the engine does not honour them     -> `out=` moves correctly and the SCREEN does not
#
# Both halves are measured here: the lanes come from the log, the screen from a template match of a
# pure translation (same hand orientation throughout, so the gun's silhouette is unchanged and the
# correlation is trustworthy).
param(
    [string]$GameDir = 'C:\Games\Sunrise',
    [string]$Tag = 'pg',
    # Kept modest so the gun stays inside the template box and within the search radius: 0.15 m is
    # about 310 px, against a search window of 614.
    [double]$Step = 0.15,
    [int]$SettleSeconds = 4
)
$ErrorActionPreference = 'Stop'
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'lib\GameIO.ps1')
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$shots = Join-Path $repo 'build\shots'
$log = Join-Path $GameDir 'bin\x64\Sunrise\logs\sunrise.log'
$inv = [System.Globalization.CultureInfo]::InvariantCulture

function Read-LogLines {
    $stream = [IO.File]::Open($log, 'Open', 'Read', 'ReadWrite')
    try {
        $reader = New-Object IO.StreamReader($stream)
        try { return $reader.ReadToEnd() -split "`r?`n" } finally { $reader.Dispose() }
    } finally { $stream.Dispose() }
}
function Send-Weapon {
    param([string[]]$Lines)
    [IO.File]::WriteAllText((Join-Path $GameDir 'SVR_Weapon.txt'), (($Lines -join "`n") + "`n"),
        (New-Object System.Text.UTF8Encoding($false)))
    Start-Sleep -Seconds 3
}
function Fmt([double]$v) { return $v.ToString($inv) }

# The lanes the module last wrote, straight out of the transform dump.
function Get-Lanes {
    $line = (Read-LogLines) | Select-String 'ev=vr\.weapon xform caller' | Select-Object -Last 1
    if (-not $line) { return $null }
    if ($line.Line -match 'out=([-\d.]+),([-\d.]+),([-\d.]+)') {
        return @([double]::Parse($Matches[1], $inv), [double]::Parse($Matches[2], $inv),
            [double]::Parse($Matches[3], $inv))
    }
    return $null
}

$zero = @{ HeadYaw = 0; HeadPitch = 0; HeadX = 0; HeadY = 0; HeadZ = 0
    HandYaw = 0; HandPitch = 0; HandRoll = 0; HandX = 0; HandY = 0; HandZ = 0
    LHandYaw = 0; LHandPitch = 0; LHandRoll = 0; LHandX = 0; LHandY = 0; LHandZ = 0 }

$index = 0
function Capture {
    param([string]$Name)
    $script:index++
    Set-MockInput @zero | Out-Null
    Start-Sleep -Seconds $SettleSeconds
    [void](Assert-GameFocus)
    $file = "{0}_{1:d2}_{2}.png" -f $Tag, $script:index, $Name
    Save-Shot (Join-Path $shots $file) | Out-Null
    return $file
}
function Measure-Shift {
    param([string]$Base, [string]$Shot)
    $env:SVR_CLIENT_RECT = Export-ClientRect
    $out = python (Join-Path $PSScriptRoot 'weapon_shift.py') $shots $Base $Shot
    $line = $out | Select-Object -Last 1
    if ($line -match '^\S+\s+([+-]?\d+)\s+([+-]?\d+)\s+([\d.]+)') {
        return @{ dx = [int]$Matches[1]; dy = [int]$Matches[2]; peak = [double]::Parse($Matches[3], $inv) }
    }
    throw "could not parse weapon_shift output: $line"
}

if (-not (Get-GameProcess)) { throw 'game is not running; run Run-Patrol.ps1 first' }
[void](Assert-GameFocus)

Send-Weapon @('install', 'block on', 'getter D5D832 hand', 'xform clear', 'xform lanes 4 5 6',
    'xform delta on', 'anchor palm', 'pivot 0 0 0', 'xform dump on')
Start-Sleep -Seconds 2

$base = Capture 'pivot_zero'
$laneZero = Get-Lanes
Write-Output ("lanes at pivot 0 0 0        : {0}" -f (($laneZero | ForEach-Object { '{0:f4}' -f $_ }) -join ', '))

$names = @('fwd', 'right', 'up')
$rows = New-Object System.Collections.Generic.List[object]
for ($axis = 0; $axis -lt 3; $axis++) {
    $p = @(0.0, 0.0, 0.0)
    $p[$axis] = $Step
    Send-Weapon @(('pivot {0} {1} {2}' -f (Fmt $p[0]), (Fmt $p[1]), (Fmt $p[2])))
    $shot = Capture ('pivot_' + $names[$axis])
    $lanes = Get-Lanes
    $dLane = @(($lanes[0] - $laneZero[0]), ($lanes[1] - $laneZero[1]), ($lanes[2] - $laneZero[2]))
    $laneMag = [Math]::Sqrt($dLane[0] * $dLane[0] + $dLane[1] * $dLane[1] + $dLane[2] * $dLane[2])
    $m = Measure-Shift $base $shot
    $pixMag = [Math]::Sqrt([Math]::Pow($m.dx, 2) + [Math]::Pow($m.dy, 2))
    $rows.Add([pscustomobject]@{
            Axis = $names[$axis]
            LaneDelta = (($dLane | ForEach-Object { '{0:f4}' -f $_ }) -join ',')
            # Must equal $Step: the pivot is a unit-basis vector scaled by Step, so a correct write
            # moves the lanes by exactly Step metres however the basis is oriented.
            LaneMag = ('{0:f4}' -f $laneMag)
            ExpectedLaneMag = ('{0:f4}' -f $Step)
            dx = $m.dx; dy = $m.dy; PixMag = [int]$pixMag
            # 0.08 m was measured at 166 px, so 2075 px/m is the standing calibration.
            ExpectedPix = [int]($Step * 2075)
            Peak = ('{0:f2}' -f $m.peak)
        })
}
Send-Weapon @('pivot 0 0 0', 'xform dump off')
Set-MockInput @zero | Out-Null

Write-Output ''
Write-Output '== does a pivot of one basis vector move the lanes by exactly Step metres? =='
Write-Output '== and does the screen follow at the calibrated 2075 px/m? =='
$rows | Format-Table -AutoSize
Write-Output ("shots -> {0}\{1}_*.png" -f $shots, $Tag)
