# Measures the pivot by a one-dimensional scan, and it is the right shape of experiment.
#
# The earlier least-squares Jacobian was the wrong tool, for a reason worth recording. The pivot's
# forward axis turned out to point along the camera's own view direction -- of course it does, the
# hand points where the player looks -- so moving it changes the weapon's DEPTH, and a screen-space
# measurement is nearly blind to depth: 0.15 m forward moved the gun 88 px where the same 0.15 m
# sideways moved it 253. The Jacobian was singular in exactly the axis that mattered.
#
# What IS strongly observable is the thing the pivot is supposed to fix: how far a hand ROTATION
# translates the weapon. Rotating a lever arm of length |d + pivot| about the wrist turns depth into
# sideways motion, which the screen sees perfectly well. So:
#
#   residual(p) = 2*sin(angle/2) * |d + p| * (px per metre)  +  a constant from the palm's own arc
#
# a V with its minimum at p = -|d|. Scanning it and taking the minimum is direct, needs no
# linearisation, and cannot be fooled by a singular direction. The prediction is strong and
# falsifiable: the residual should fall from about 197 px to the palm's own contribution of ~30.
#
# Roll is carried through the scan as a CONTROL. `d` has no measurable component off the aim axis,
# so rolling about that axis should translate the weapon by nothing at every point of the scan. If
# the roll number moves while the yaw number improves, the model is wrong and the yaw minimum means
# something other than what it claims.
param(
    [string]$GameDir = 'C:\Games\Sunrise',
    [string]$Tag = 'sp',
    # Candidate forward pivots, in metres. Bracketing the independently measured |d| = 0.33 m.
    # The scan MUST cross the vertex. Between 0 and -|d| the residual is linear in the pivot, so a
    # fit there cannot separate the slope from the measurement's constant bias -- and the bias is
    # large: a yawed gun foreshortens, which shifts the correlation peak by around 100 px with no
    # translation behind it at all. Past the vertex the slope changes SIGN, and that is what pins
    # |d| down without needing to know the bias.
    [double[]]$Forward = @(0, -0.12, -0.24, -0.33, -0.42, -0.50, -0.58),
    [double]$Angle = 15,
    # The mock's synthetic hand sits only 0.3 m in front of the head and 0.2 m below it, which is
    # closer than anyone holds a controller. With the absolute placement that puts the weapon about
    # 0.23 m from the eye: it fills the frame, the near plane starts clipping it, and the template
    # match collapses (peak 0.30) exactly at the pivot values worth measuring. So the hand is held
    # out to a natural 0.45 m forward and 0.35 m below for these tests.
    #
    # As an offset rather than a change to the mock's default on purpose: Test-HandPose.ps1 asserts
    # the default rest offset of (0.30, -0.20, -0.20) against hand-computed values, and moving the
    # fixture would break a passing test to make this one convenient.
    # Held well out, and that is a constraint of the experiment rather than taste. The pivot moves
    # the weapon along the view axis, so a scan that reaches past -|d| pulls it towards the eye: at
    # the mock's default hold the weapon crossed the near plane and stopped being matchable exactly
    # where the vertex was. 0.60 m forward leaves room to scan to -0.58 and still have the weapon
    # 0.35 m away.
    [double]$HoldY = -0.20,
    [double]$HoldZ = -0.30,
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

$zero = @{ HeadYaw = 0; HeadPitch = 0; HeadX = 0; HeadY = 0; HeadZ = 0
    HandYaw = 0; HandPitch = 0; HandRoll = 0; HandX = 0; HandY = $HoldY; HandZ = $HoldZ
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
        if ($peak -lt $MinPeak) {
            throw ("no trustworthy match for {0} vs {1}: peak {2:f3}" -f $Base, $Shot, $peak)
        }
        return @{ dx = [int]$Matches[1]; dy = [int]$Matches[2]; peak = $peak }
    }
    throw "could not parse weapon_shift output: $line"
}
function Mag($m) { return [Math]::Sqrt([Math]::Pow($m.dx, 2) + [Math]::Pow($m.dy, 2)) }

if (-not (Get-GameProcess)) { throw 'game is not running; run Run-Patrol.ps1 first' }
[void](Assert-GameFocus)

# Out of the idle pose before anything is measured -- see Clear-WeaponIdle.
[void](Clear-WeaponIdle)
Send-Weapon @('install', 'block on', 'getter D5D832 hand', 'xform clear', 'xform lanes 4 5 6',
    'xform delta on', 'anchor palm')

$rows = New-Object System.Collections.Generic.List[object]
foreach ($p in $Forward) {
    Send-Weapon @(('pivot {0} 0 0' -f (Fmt $p)))
    $slug = 'f{0}' -f ([int]([Math]::Abs($p) * 100))
    $hold = Capture ($slug + '_hold')
    $yaw = Capture ($slug + '_yaw') @{ HandYaw = $Angle }
    $roll = Capture ($slug + '_roll') @{ HandRoll = $Angle }
    # One unmatchable point must not abandon the scan: the whole reason for scanning is that some
    # of the range is hostile to the instrument, and the rest of the curve is still worth having.
    try {
        $y = Measure-Shift $hold $yaw
        $r = Measure-Shift $hold $roll -Rotate $Angle
    } catch {
        Write-Output ("  pivot fwd {0,6:f3}   SKIPPED: {1}" -f $p, $_.Exception.Message)
        continue
    }
    $rows.Add([pscustomobject]@{
            # Invariant, not '{0:f3}': this machine formats 0.4 as "0,4", and parsing that back
            # with InvariantCulture reads the comma as a THOUSANDS separator and returns 400.
            # The summary line printed "|d| implied = 400,000 m" before this was fixed.
            PivotFwd = ([double]$p).ToString('F3', $inv)
            YawDx = $y.dx; YawDy = $y.dy; YawMag = [int](Mag $y); YawPeak = ('{0:f2}' -f $y.peak)
            RollMag = [int](Mag $r); RollPeak = ('{0:f2}' -f $r.peak)
        })
    Write-Output ("  pivot fwd {0,6:f3}   yaw {1,5} {2,5} px |{3,4}|  (peak {4:f2})   roll |{5,4}| (peak {6:f2})" -f
        $p, $y.dx, $y.dy, [int](Mag $y), $y.peak, [int](Mag $r), $r.peak)
}

Send-Weapon @('pivot 0 0 0')
Set-MockInput @zero | Out-Null

Write-Output ''
Write-Output '== how far a 15 degree hand YAW translates the weapon, against the forward pivot =='
Write-Output '== ROLL is the control: it must stay near zero throughout =='
$rows | Format-Table -AutoSize
if ($rows.Count -eq 0) { throw 'every point of the scan failed to match; look at the captures' }
$best = $rows | Sort-Object { [int]$_.YawMag } | Select-Object -First 1
Write-Output ("minimum at pivot fwd = {0}  with {1} px of yaw travel" -f $best.PivotFwd, $best.YawMag)
Write-Output ("|d| implied            = {0:f3} m   (independently measured at 0.33 m in RESEARCH.md)" -f
    [Math]::Abs([double]::Parse($best.PivotFwd, $inv)))
$csv = Join-Path $shots ("{0}_scan.csv" -f $Tag)
$rows | Export-Csv -NoTypeInformation -Path $csv
Write-Output ''
Write-Output '== fitting the V: |d| is where the slope changes sign =='
python (Join-Path $PSScriptRoot 'pivot_fit.py') $csv
Write-Output ("shots -> {0}\{1}_*.png" -f $shots, $Tag)
