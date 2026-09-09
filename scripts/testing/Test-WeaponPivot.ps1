# The two regressions the old suite walked straight through, plus the calibration that makes the
# first one passable at all.
#
#   PURE ROTATION MUST NOT TRANSLATE. Hold the controller's position, turn it, and the weapon must
#       spin in place. It did not: the engine draws the weapon at `camera + R(q) * d + lanes` and
#       nothing cancelled `R(q) * d`, so the centre of rotation was the EYE, 0.33 m behind both the
#       gun and the hands. A 90 degree wrist roll moved the gun about 0.47 m.
#
#   ARTIFICIAL TURNING MUST NOT TRANSLATE. Hold the controller still, turn the room anchor, and the
#       weapon must stay exactly where it is on screen. It did not: the rest reference was a
#       snapshot of an ALREADY-FOLDED hand offset, so it went stale by the fold angle and grew into
#       over a metre of phantom translation at 180 degrees.
#
# Neither could be measured before this script: roll is the axis the symptom lives in and the mock
# had no roll field, and nothing in the suite ever turned while holding the hand still.
#
# A third section is not optional either: with the pivot cancelled it is trivially easy to pass the
# first two by breaking the response altogether, so the translation checks are here to prove the
# weapon still moves when the hand really moves.
#
# Wants the game already in a first-person destination with F9 on: run Run-Patrol.ps1 first.
param(
    [string]$GameDir = 'C:\Games\Sunrise',
    [string]$Tag = 'wp',
    # Which of the controller's two OpenXR poses anchors the weapon. `palm` is the grip pose, the
    # palm centroid, which is the point a wrist actually rotates about. `aim` is the pointing ray's
    # origin, which on Touch hardware floats several centimetres in front of the hand -- kept only
    # so the difference can be measured instead of argued about.
    [ValidateSet('palm', 'aim')][string]$Anchor = 'palm',
    [double[]]$Pivot = @(0, 0, 0),
    # Measure and solve for the pivot rather than trusting the one passed in.
    [switch]$Solve,
    [int]$Iterations = 2,
    # How far each axis of the pivot is probed, in metres, when solving. Big enough to move the gun
    # well clear of the noise floor, small enough that the affine model still holds.
    [double]$Probe = 0.12,
    # The rotation used for the measurement, in degrees. Deliberately small: template matching is
    # NOT rotation invariant, so a big roll turns the gun's image enough to spoil the correlation
    # peak, and the peak's position is the measurement. 15 degrees on a 0.33 m lever arm is about
    # 180 px of travel -- unmistakable -- while leaving the match trustworthy.
    [double]$Angle = 15,
    [double]$TurnDegrees = 180,
    # Where the hand is held, as an offset on the mock's default (0.2 right, 0.2 below, 0.3 forward
    # of the eye). The default is closer and lower than anyone holds a controller, and with the
    # pivot correct the weapon's origin sits ON the palm -- so at the default hold the weapon ends
    # up below the field of view and cannot be measured at all.
    [double]$HoldX = -0.05,
    [double]$HoldY = 0.05,
    [double]$HoldZ = -0.20,
    # The template box weapon_shift.py takes its template from, as fractions of the client area.
    # It has to be told: the default box suits the engine's own viewmodel spot, and a box holding
    # only scenery reports a confident 0 px shift, which is a PASS for the wrong reason.
    [string]$TemplateBox = '0.52,0.62,0.86,1.0',
    [int]$SettleSeconds = 4,
    # Skip the calibration and the translation sanity checks; just run the two regressions.
    [switch]$Quick
)
$ErrorActionPreference = 'Stop'
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'lib\GameIO.ps1')
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$shots = Join-Path $repo 'build\shots'
if (-not (Test-Path $shots)) { New-Item -ItemType Directory -Force -Path $shots | Out-Null }
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
    param([string[]]$Lines, [switch]$Quiet)
    $before = ((Read-LogLines) | Select-String 'ev=vr\.weapon').Count
    [IO.File]::WriteAllText((Join-Path $GameDir 'SVR_Weapon.txt'), (($Lines -join "`n") + "`n"),
        (New-Object System.Text.UTF8Encoding($false)))
    Start-Sleep -Seconds 3
    $out = (Read-LogLines) | Select-String 'ev=vr\.weapon' | Select-Object -Skip $before |
        ForEach-Object { '    ' + ($_.Line -replace '.*client level=warn ', '') }
    if (-not $Quiet) { $out }
}

# Invariant formatting is not optional: this machine's locale writes 0.6 as "0,6", the module's
# sscanf stops at the comma and reads zero, and the command then silently does nothing -- which
# reads exactly like a broken feature.
function Fmt([double]$v) { return $v.ToString($inv) }
function Send-Pivot([double[]]$P) {
    Send-Weapon -Quiet @(('pivot {0} {1} {2}' -f (Fmt $P[0]), (Fmt $P[1]), (Fmt $P[2])))
}

# Every field explicit on every call: the mock keeps the previous value for anything omitted, so a
# partial write carries the last step's state into this one and looks like a bug in the module.
$zero = @{ HeadYaw = 0; HeadPitch = 0; HeadX = 0; HeadY = 0; HeadZ = 0
    HandYaw = 0; HandPitch = 0; HandRoll = 0; HandX = $HoldX; HandY = $HoldY; HandZ = $HoldZ
    LHandYaw = 0; LHandPitch = 0; LHandRoll = 0; LHandX = 0; LHandY = 0; LHandZ = 0 }
$env:SVR_TEMPLATE_BOX = $TemplateBox

$index = 0
$rows = New-Object System.Collections.Generic.List[object]

# Applies one input state, waits for it to reach the screen, and captures. Returns the file name.
function Capture {
    param([string]$Name, [hashtable]$Changes = @{})
    $script:index++
    $mock = $zero.Clone()
    foreach ($k in $Changes.Keys) { $mock[$k] = $Changes[$k] }
    Set-MockInput @mock | Out-Null
    Start-Sleep -Seconds $SettleSeconds
    # Throws rather than capturing whatever window happens to be in front. A shot of the wrong
    # window still template-matches, still yields a dx/dy, and is indistinguishable from a real
    # measurement -- it produced a 582 px "result" at peak 0.04 before this guard existed.
    [void](Assert-GameFocus)
    $file = "{0}_{1:d3}_{2}.png" -f $Tag, $script:index, $Name
    Save-Shot (Join-Path $shots $file) | Out-Null
    $hand = (Read-LogLines) | Select-String 'ev=vr\.xr hand' | Select-Object -Last 1
    $body = (Read-LogLines) | Select-String 'ev=vr\.body anchor' | Select-Object -Last 1
    $rows.Add([pscustomobject]@{
            Step = $script:index; Name = $Name; Shot = $file
            Hand = if ($hand) { ($hand.Line -replace '.*ev=vr\.xr hand ', '') } else { '' }
            Body = if ($body) { ($body.Line -replace '.*ev=vr\.body ', '') } else { '' }
        })
    return $file
}

<#
Records a check whose measurement may legitimately fail to match, without abandoning the run.

The sanity checks move the weapon a long way on purpose, which can take it out of the template box
-- and an unmatchable pair there is a limit of the instrument, not a failure of the module. It must
be reported as such rather than either passing quietly or killing the suite before it prints its
verdict.
#>
function Judge-Tolerant {
    param([string]$Name, [string]$Base, [string]$Shot, [string]$Rule, [double]$Limit, [double]$Rotate = 0)
    try {
        Judge $Name (Measure-Shift $Base $Shot -Rotate $Rotate) $Rule $Limit
    } catch {
        $verdicts.Add([pscustomobject]@{
                Check = $Name; dx = 0; dy = 0; Mag = 0; Peak = 'n/a'
                Rule = $Rule; Limit = [int]$Limit; Verdict = 'NO MATCH'
            })
        Write-Output ("  [ -- ] {0,-26} no trustworthy match: {1}" -f $Name, $_.Exception.Message)
    }
}

<#
Template-matches the weapon between two captures. Returns dx, dy, peak.

`Rotate` enables a search over template rotations of 0 and plus/minus that angle. Pass the roll
angle that was commanded: normalised cross-correlation is not rotation invariant, so a rolled gun
can fail to match its own unrotated template.

A peak below MinPeak is REFUSED rather than returned. The dx/dy of a failed match is not a small
error, it is a random offset from wherever the correlation happened to peak, and it looks exactly
like a real measurement -- which is how a capture of the wrong window turned into "582 px".
#>
function Measure-Shift {
    param([string]$Base, [string]$Shot, [double]$Rotate = 0, [double]$MinPeak = 0.45)
    $env:SVR_CLIENT_RECT = Export-ClientRect
    # Not $args: that is a PowerShell automatic variable holding a function's unbound arguments,
    # and shadowing it inside a function that has a param block is asking for trouble.
    $shiftArgs = @($shots, $Base, $Shot)
    if ([Math]::Abs($Rotate) -gt 0.001) {
        $shiftArgs = @(('--rotate={0}' -f ([double]$Rotate).ToString($inv))) + $shiftArgs
    }
    $out = python (Join-Path $PSScriptRoot 'weapon_shift.py') @shiftArgs
    $line = $out | Select-Object -Last 1
    if ($line -match '^\S+\s+([+-]?\d+)\s+([+-]?\d+)\s+([\d.]+)\s+([+-]?[\d.]+)') {
        $peak = [double]::Parse($Matches[3], $inv)
        if ($peak -lt $MinPeak) {
            throw ("weapon_shift found no trustworthy match for {0} vs {1}: peak {2:f3} < {3:f2}. " -f
                $Base, $Shot, $peak, $MinPeak) +
                'Either the weapon left the template box or the capture is not of the game.'
        }
        return @{ dx = [int]$Matches[1]; dy = [int]$Matches[2]; peak = $peak
            rot = [double]::Parse($Matches[4], $inv) }
    }
    throw "could not parse weapon_shift output: $line"
}

<#
One measurement of how much a PURE ROTATION translates the weapon, at the given pivot.

Two rotation axes, because one is blind to the component of the pivot along its own axis. Roll is
the axis the user noticed and the one a wrist really turns about; yaw is independent of it.
Returns the four residual pixels as an array: dx,dy under roll then dx,dy under yaw.
#>
function Measure-Residual {
    param([double[]]$P, [string]$Label)
    Send-Pivot $P | Out-Null
    $base = Capture "$Label`_hold"
    $roll = Capture "$Label`_roll" @{ HandRoll = $Angle }
    $yaw = Capture "$Label`_yaw" @{ HandYaw = $Angle }
    $r = Measure-Shift $base $roll -Rotate $Angle
    $y = Measure-Shift $base $yaw
    # Straight to stdout, bypassing the PowerShell pipeline entirely. Write-Output cannot be used
    # because this function returns a value and would return the text alongside it; Write-Host is
    # not reliably captured when the script runs as a background task, and these numbers ARE the
    # measurement, so losing them is not an option.
    # The extra parentheses are load bearing: inside a method call's argument list the commas are
    # ARGUMENT separators, so without them `-f` gets one operand and the rest become further
    # arguments to WriteLine -- which fails as a format error, not as a syntax error.
    [Console]::Out.WriteLine(("     roll {0,6} {1,6} px (peak {2:f2})   yaw {3,6} {4,6} px (peak {5:f2})" -f
            $r.dx, $r.dy, $r.peak, $y.dx, $y.dy, $y.peak))
    return @{ v = @($r.dx, $r.dy, $y.dx, $y.dy); peak = [Math]::Min($r.peak, $y.peak) }
}

if (-not (Get-GameProcess)) { throw 'game is not running; run Run-Patrol.ps1 first' }
[void](Assert-GameFocus)
if (-not ((Read-LogLines) | Select-String 'ev=vr\.xr init result=ok')) {
    throw 'no OpenXR session in the log -- press F9 in world first'
}

Write-Output "== configuring: anchor=$Anchor, angle=$Angle deg, turn=$TurnDegrees deg =="
# Out of the idle pose before anything is measured -- see Clear-WeaponIdle.
[void](Clear-WeaponIdle)
Send-Weapon @('install', 'block on', 'getter D5D832 hand', 'xform clear', 'xform lanes 4 5 6',
    'xform delta on', "anchor $Anchor")
Send-Weapon @('report')

# $current, not $pivot: `$pivot` IS the [double[]]$Pivot parameter under PowerShell's
# case-insensitive variable names, and relying on the types happening to match is how the sibling
# bug above went unnoticed.
$current = @([double]$Pivot[0], [double]$Pivot[1], [double]$Pivot[2])

if ($Solve -and -not $Quick) {
    for ($iteration = 1; $iteration -le $Iterations; $iteration++) {
        Write-Output ''
        Write-Output ("== CALIBRATION pass {0}: probing each axis of the pivot ==" -f $iteration)
        $csv = Join-Path $shots ("{0}_pivot_pass{1}.csv" -f $Tag, $iteration)
        $probeRows = New-Object System.Collections.Generic.List[object]
        Write-Output ("  base   pivot {0:f3} {1:f3} {2:f3}" -f $current[0], $current[1], $current[2])
        $m = Measure-Residual $current ("p{0}_base" -f $iteration)
        $probeRows.Add([pscustomobject]@{ probe = 'base'; pf = (Fmt $current[0]); pr = (Fmt $current[1]); pu = (Fmt $current[2])
                dx_a = $m.v[0]; dy_a = $m.v[1]; dx_b = $m.v[2]; dy_b = $m.v[3] })
        $names = @('fwd', 'right', 'up')
        for ($axis = 0; $axis -lt 3; $axis++) {
            # NOT $probe: that name is the [double]$Probe parameter, and PowerShell variable
            # names are case-insensitive, so assigning an array to it throws a type error.
            $stepped = @($current[0], $current[1], $current[2])
            $stepped[$axis] = $current[$axis] + $Probe
            Write-Output ("  {0,-6} pivot {1:f3} {2:f3} {3:f3}" -f $names[$axis], $stepped[0], $stepped[1], $stepped[2])
            $m = Measure-Residual $stepped ("p{0}_{1}" -f $iteration, $names[$axis])
            $probeRows.Add([pscustomobject]@{ probe = $names[$axis]; pf = (Fmt $stepped[0]); pr = (Fmt $stepped[1]); pu = (Fmt $stepped[2])
                    dx_a = $m.v[0]; dy_a = $m.v[1]; dx_b = $m.v[2]; dy_b = $m.v[3] })
        }
        $probeRows | Export-Csv -NoTypeInformation -Path $csv
        $solved = python (Join-Path $PSScriptRoot 'pivot_solve.py') $csv
        $solved | ForEach-Object { "  $_" }
        $answer = $solved | Where-Object { $_ -match '^PIVOT ' } | Select-Object -Last 1
        if (-not $answer) { throw 'pivot_solve produced no solution' }
        $parts = ($answer -split '\s+')
        $current = @([double]::Parse($parts[1], $inv), [double]::Parse($parts[2], $inv),
            [double]::Parse($parts[3], $inv))
        Write-Output ("  -> pivot {0:f4} {1:f4} {2:f4}" -f $current[0], $current[1], $current[2])
    }
}

Write-Output ''
Write-Output ("== applying pivot {0:f4} {1:f4} {2:f4} ==" -f $current[0], $current[1], $current[2])
Send-Pivot $current

Write-Output ''
Write-Output '== NOISE FLOOR: nothing changes between these two =='
$rest = Capture 'rest'
$restAgain = Capture 'rest_again'
$floor = Measure-Shift $rest $restAgain
Write-Output ("     {0,6} {1,6} px (peak {2:f2})  <- every number below is judged against this" -f
    $floor.dx, $floor.dy, $floor.peak)
$noise = [Math]::Sqrt([Math]::Pow($floor.dx, 2) + [Math]::Pow($floor.dy, 2))

$verdicts = New-Object System.Collections.Generic.List[object]
# Records one check and prints it. Returns nothing on purpose: the verdict lives in $verdicts, and
# a return value would have to be discarded at every call site, which would discard the print with
# it.
function Judge {
    param([string]$Name, [hashtable]$M, [string]$Rule, [double]$Limit)
    $mag = [Math]::Sqrt([Math]::Pow($M.dx, 2) + [Math]::Pow($M.dy, 2))
    $pass = if ($Rule -eq 'still') { $mag -le $Limit } else { $mag -ge $Limit }
    $verdict = if ($pass) { 'PASS' } else { 'FAIL' }
    $verdicts.Add([pscustomobject]@{
            Check = $Name; dx = $M.dx; dy = $M.dy; Mag = [int]$mag; Peak = ('{0:f2}' -f $M.peak)
            Rule = $Rule; Limit = [int]$Limit; Verdict = $verdict
        })
    Write-Output ("  [{0}] {1,-26} {2,6} {3,6} px  |{4,4}|  peak {5:f2}   {6} {7}" -f
        $verdict, $Name, $M.dx, $M.dy, [int]$mag, $M.peak, $Rule, [int]$Limit)
}

# The bar for "did not move". Three times the scene's own noise, floored at 25 px so a freakishly
# quiet pair cannot set an impossible standard. 25 px is about 0.012 m at the weapon's distance.
$still = [Math]::Max(25.0, 3.0 * $noise)
# The bar for "really did move". The amplitudes below are chosen to clear this by a wide margin;
# it exists to catch a pivot that passes the two regressions by killing the response altogether.
$moved = 60.0

Write-Output ''
Write-Output '== REGRESSION 1: pure rotation must not translate the weapon =='
foreach ($case in @(
        @{ n = 'roll_pos'; c = @{ HandRoll = $Angle }; r = $Angle },
        @{ n = 'roll_neg'; c = @{ HandRoll = -$Angle }; r = $Angle },
        @{ n = 'roll_big'; c = @{ HandRoll = (2 * $Angle) }; r = (2 * $Angle) },
        @{ n = 'yaw_pos'; c = @{ HandYaw = $Angle }; r = 0 },
        @{ n = 'yaw_neg'; c = @{ HandYaw = -$Angle }; r = 0 },
        @{ n = 'pitch_pos'; c = @{ HandPitch = $Angle }; r = 0 },
        @{ n = 'pitch_neg'; c = @{ HandPitch = -$Angle }; r = 0 })) {
    $shot = Capture $case.n $case.c
    Judge $case.n (Measure-Shift $rest $shot -Rotate $case.r) 'still' $still
}

Write-Output ''
Write-Output '== REGRESSION 2: artificial turning must not translate the weapon =='
# The hand is held at the rest pose throughout; only the room anchor moves. The world swings round
# behind the gun, so the gun's SCREEN position is the invariant, which is what is measured.
$half = $TurnDegrees / 2.0
Send-Weapon -Quiet @(('turn {0}' -f (Fmt $half)))
$turnA = Capture ('turn_{0:d}' -f [int]$half)
Judge ('turn_{0:d}' -f [int]$half) (Measure-Shift $rest $turnA) 'still' $still
Send-Weapon -Quiet @(('turn {0}' -f (Fmt $half)))
$turnB = Capture ('turn_{0:d}' -f [int]$TurnDegrees)
Judge ('turn_{0:d}' -f [int]$TurnDegrees) (Measure-Shift $rest $turnB) 'still' $still
# And with the anchor turned right round, rolling the wrist must STILL not translate: the two bugs
# were independent, and this is the one step that would have caught either of them on its own.
$turnRoll = Capture 'turn_then_roll' @{ HandRoll = $Angle }
Judge 'turn_then_roll' (Measure-Shift $turnB $turnRoll -Rotate $Angle) 'still' $still
Send-Weapon -Quiet @(('turn {0}' -f (Fmt (-$TurnDegrees))))
$back = Capture 'turn_back'
Judge 'turn_back' (Measure-Shift $rest $back) 'still' $still

if (-not $Quick) {
    Write-Output ''
    Write-Output '== SANITY: the weapon must still MOVE when the hand really moves =='
    foreach ($case in @(
            @{ n = 'hand_right'; c = @{ HandX = 0.1 } },
            @{ n = 'hand_up'; c = @{ HandY = 0.1 } },
            @{ n = 'hand_back'; c = @{ HandZ = 0.1 } })) {
        $shot = Capture $case.n $case.c
        Judge-Tolerant $case.n $back $shot 'moves' $moved
    }
    Write-Output ''
    Write-Output '== SANITY: the head moving must leave the weapon in the WORLD =='
    # Which reads on screen as the gun sliding the opposite way. Same bar as above: it has to move.
    foreach ($case in @(
            @{ n = 'head_right'; c = @{ HeadX = 0.12 } },
            @{ n = 'head_lean_in'; c = @{ HeadZ = -0.15 } })) {
        $shot = Capture $case.n $case.c
        Judge-Tolerant $case.n $back $shot 'moves' $moved
    }
}

Set-MockInput @zero | Out-Null
$rows | Export-Csv -NoTypeInformation -Path (Join-Path $shots ("{0}_steps.csv" -f $Tag))
$verdicts | Export-Csv -NoTypeInformation -Path (Join-Path $shots ("{0}_verdicts.csv" -f $Tag))

Write-Output ''
Write-Output '== VERDICT =='
$verdicts | Format-Table -AutoSize
$failed = @($verdicts | Where-Object { $_.Verdict -eq 'FAIL' })
$unmatched = @($verdicts | Where-Object { $_.Verdict -eq 'NO MATCH' })
Write-Output ("pivot     {0:f4} {1:f4} {2:f4}   |pivot| {3:f4} m" -f $current[0], $current[1], $current[2],
    [Math]::Sqrt($current[0] * $current[0] + $current[1] * $current[1] + $current[2] * $current[2]))
Write-Output ("noise     {0:f1} px    still bar {1:f1} px" -f $noise, $still)
Write-Output ("checks    {0} passed, {1} failed, {2} unmeasurable, of {3}" -f
    ($verdicts.Count - $failed.Count - $unmatched.Count), $failed.Count, $unmatched.Count, $verdicts.Count)
Write-Output ("shots  -> {0}\{1}_*.png" -f $shots, $Tag)
Send-Weapon @('report')
if ($failed.Count -gt 0) { exit 1 }
