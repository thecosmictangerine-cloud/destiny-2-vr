# Solves the two pivot components that a WRIST ROLL can see, which is the axis the user complained
# about, and does it with the only rotation this scene measures without bias.
#
# Why roll and not yaw or pitch: a roll turns the weapon in the image plane and PRESERVES its
# silhouette, so cross-correlation (searching template rotations of 0 and plus/minus the roll angle)
# reports the translation and nothing else. A yaw or pitch foreshortens the gun, and correlation
# then answers with the offset that best fits a changed shape -- worth about 100 px here, the same
# size as the effect. So yaw and pitch can say the pivot got better; only roll can say it is right.
#
# What roll can and cannot see: rolling about the aim axis is blind to the pivot's component ALONG
# that axis, exactly as a spinning rod cannot reveal its own length. So `fwd` is held at the value
# measured independently (0.33 m, from 0.08 m of lane travel subtending 13.5 degrees, RESEARCH.md)
# and the two perpendicular components are solved for here.
#
# The problem is affine in those two: rolling from R1 to R2 displaces the weapon by
# (R2 - R1) * (d + pivot), so probing each axis once gives the 2x2 Jacobian by finite differences
# and one linear solve gives the answer. That is three measurements, not a search.
#
# Two earlier attempts at this failed for reasons that are now guarded rather than remembered:
# captures of the wrong window (Assert-GameFocus refuses to shoot anything but the game) and a
# template box that no longer contained the weapon, which returns a confident 0 px at peaks up to
# 0.97 (SVR_TEMPLATE_BOX, and a minimum-peak guard).
param(
    [string]$GameDir = 'C:\Games\Sunrise',
    [string]$Tag = 'pr',
    [double]$PivotFwd = -0.33,
    [double]$Right = 0,
    [double]$Up = 0,
    [double]$Probe = 0.08,
    [double]$Angle = 20,
    [int]$Iterations = 2,
    [double]$HoldX = -0.05,
    [double]$HoldY = 0.05,
    [double]$HoldZ = -0.20,
    [string]$TemplateBox = '0.52,0.62,0.86,1.0',
    [int]$SettleSeconds = 3
)
$ErrorActionPreference = 'Stop'
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'lib\GameIO.ps1')
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$shots = Join-Path $repo 'build\shots'
$inv = [System.Globalization.CultureInfo]::InvariantCulture
$env:SVR_TEMPLATE_BOX = $TemplateBox

function Send-Weapon {
    param([string[]]$Lines)
    [IO.File]::WriteAllText((Join-Path $GameDir 'SVR_Weapon.txt'), (($Lines -join "`n") + "`n"),
        (New-Object System.Text.UTF8Encoding($false)))
    Start-Sleep -Seconds 3
}
function Fmt([double]$v) { return $v.ToString($inv) }

$zero = @{ HeadYaw = 0; HeadPitch = 0; HeadX = 0; HeadY = 0; HeadZ = 0
    HandYaw = 0; HandPitch = 0; HandRoll = 0; HandX = $HoldX; HandY = $HoldY; HandZ = $HoldZ
    LHandYaw = 0; LHandPitch = 0; LHandRoll = 0; LHandX = 0; LHandY = 0; LHandZ = 0 }

$index = 0
function Capture {
    param([string]$Name, [hashtable]$Changes = @{})
    $script:index++
    $mock = $zero.Clone()
    foreach ($k in $Changes.Keys) { $mock[$k] = $Changes[$k] }
    Set-MockInput @mock | Out-Null
    Start-Sleep -Seconds $SettleSeconds
    [void](Assert-GameFocus)
    $file = "{0}_{1:d3}_{2}.png" -f $Tag, $script:index, $Name
    Save-Shot (Join-Path $shots $file) | Out-Null
    return $file
}
function Measure-Shift {
    param([string]$Base, [string]$Shot, [double]$Rotate = 0, [double]$MinPeak = 0.45)
    $env:SVR_CLIENT_RECT = Export-ClientRect
    $a = @($shots, $Base, $Shot)
    if ([Math]::Abs($Rotate) -gt 0.001) { $a = @(('--rotate={0}' -f (Fmt $Rotate))) + $a }
    $out = python (Join-Path $PSScriptRoot 'weapon_shift.py') @a
    $line = $out | Select-Object -Last 1
    if ($line -match '^\S+\s+([+-]?\d+)\s+([+-]?\d+)\s+([\d.]+)') {
        $peak = [double]::Parse($Matches[3], $inv)
        if ($peak -lt $MinPeak) { throw ("no trustworthy match {0} vs {1}: peak {2:f3}" -f $Base, $Shot, $peak) }
        return @{ dx = [int]$Matches[1]; dy = [int]$Matches[2]; peak = $peak }
    }
    throw "could not parse weapon_shift output: $line"
}

# How far a roll translates the weapon, both signs averaged. Both signs on purpose: any residual
# shape bias is roughly odd in the roll angle, so averaging the two cancels most of what is left.
function Measure-Roll {
    param([double[]]$P, [string]$Label)
    Send-Weapon @(('pivot {0} {1} {2}' -f (Fmt $P[0]), (Fmt $P[1]), (Fmt $P[2])))
    $hold = Capture ($Label + '_hold')
    $plus = Capture ($Label + '_rollp') @{ HandRoll = $Angle }
    $minus = Capture ($Label + '_rollm') @{ HandRoll = -$Angle }
    $a = Measure-Shift $hold $plus -Rotate $Angle
    $b = Measure-Shift $hold $minus -Rotate $Angle
    $dx = ($a.dx - $b.dx) / 2.0
    $dy = ($a.dy - $b.dy) / 2.0
    [Console]::Out.WriteLine(("     pivot {0,6:f3} {1,6:f3} {2,6:f3}   roll+ {3,5} {4,5}  roll- {5,5} {6,5}  ->  half-difference {7,7:f1} {8,7:f1} px  (peaks {9:f2}/{10:f2})" -f
            $P[0], $P[1], $P[2], $a.dx, $a.dy, $b.dx, $b.dy, $dx, $dy, $a.peak, $b.peak))
    return @{ v = @($dx, $dy); peak = [Math]::Min($a.peak, $b.peak) }
}

if (-not (Get-GameProcess)) { throw 'game is not running; run Run-Patrol.ps1 first' }
[void](Assert-GameFocus)
[void](Clear-WeaponIdle)
Send-Weapon @('install', 'block on', 'getter D5D832 hand', 'xform clear', 'xform lanes 4 5 6',
    'xform delta on', 'anchor palm')

$current = @([double]$PivotFwd, [double]$Right, [double]$Up)
for ($pass = 1; $pass -le $Iterations; $pass++) {
    Write-Output ''
    Write-Output ("== PASS {0}: solving the right and up components from roll ==" -f $pass)
    $base = Measure-Roll $current ("p{0}_base" -f $pass)
    $stepR = @($current[0], ($current[1] + $Probe), $current[2])
    $mR = Measure-Roll $stepR ("p{0}_right" -f $pass)
    $stepU = @($current[0], $current[1], ($current[2] + $Probe))
    $mU = Measure-Roll $stepU ("p{0}_up" -f $pass)

    # Jacobian columns by finite difference, then a 2x2 solve of  J * delta = -residual.
    $j00 = ($mR.v[0] - $base.v[0]) / $Probe
    $j10 = ($mR.v[1] - $base.v[1]) / $Probe
    $j01 = ($mU.v[0] - $base.v[0]) / $Probe
    $j11 = ($mU.v[1] - $base.v[1]) / $Probe
    $det = $j00 * $j11 - $j01 * $j10
    Write-Output ("  jacobian  [{0,8:f0} {1,8:f0}]  det {2:e2}" -f $j00, $j01, $det)
    Write-Output ("            [{0,8:f0} {1,8:f0}]" -f $j10, $j11)
    if ([Math]::Abs($det) -lt 1e-3) {
        Write-Output '  jacobian is singular -- roll cannot see these axes here; stopping'
        break
    }
    $rx = -$base.v[0]
    $ry = -$base.v[1]
    $dRight = ($rx * $j11 - $ry * $j01) / $det
    $dUp = ($ry * $j00 - $rx * $j10) / $det
    $current = @($current[0], ($current[1] + $dRight), ($current[2] + $dUp))
    Write-Output ("  -> pivot {0:f4} {1:f4} {2:f4}   (moved right {3:+0.000}, up {4:+0.000})" -f
        $current[0], $current[1], $current[2], $dRight, $dUp)
}

Write-Output ''
Write-Output '== verifying at the solution =='
$final = Measure-Roll $current 'verify'
$mag = [Math]::Sqrt($final.v[0] * $final.v[0] + $final.v[1] * $final.v[1])
Send-Weapon @(('pivot {0} {1} {2}' -f (Fmt $current[0]), (Fmt $current[1]), (Fmt $current[2])))
Set-MockInput @zero | Out-Null

Write-Output ''
Write-Output ("PIVOT     {0:f4} {1:f4} {2:f4}" -f $current[0], $current[1], $current[2])
Write-Output ("|pivot|   {0:f4} m   (RESEARCH.md measured the engine's viewmodel offset at 0.33 m)" -f
    [Math]::Sqrt($current[0] * $current[0] + $current[1] * $current[1] + $current[2] * $current[2]))
Write-Output ("roll residual at the solution: {0:f1} px" -f $mag)
Write-Output ("shots -> {0}\{1}_*.png" -f $shots, $Tag)
