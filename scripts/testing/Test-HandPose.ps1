# Axis-by-axis check of the controller aim pose, in orbit, against values worked out by hand.
#
# Why this is worth a dedicated script: every later stage of F3 is built on this conversion, and a
# sign error here is invisible downstream -- the weapon would simply move the wrong way and look
# like an engine problem. So each axis and each rotation is driven alone and compared with a
# number, not with "it moved".
#
# The basis: OpenXR is +X right, +Y up, -Z forward, metres. The game is X forward, Z up, and
# therefore +Y LEFT, so `to_game(v) = {-v.z, -v.x, v.y}`. The mock puts the right hand at
# (0.2, 1.4, -0.3) and the eye midpoint at (0, 1.6, 0), and the recentre origin is that midpoint,
# so with every input at zero the right hand's offset in game units must be
#   x = -(-0.3 - 0) = +0.30   (0.3 forward)
#   y = -( 0.2 - 0) = -0.20   (0.2 right, because +Y is left)
#   z =  ( 1.4 - 1.6) = -0.20 (0.2 below eye level)
#
# Run after Run-Orbit.ps1 -NoToggle has left the game in orbit with F9 already pressed, or on its
# own: it toggles F9 itself if the log shows no session.
param(
    [string]$GameDir = 'C:\Games\Sunrise',
    [double]$Tolerance = 0.02,
    [int]$SettleSeconds = 4
)
$ErrorActionPreference = 'Stop'
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'lib\GameIO.ps1')
$log = Join-Path $GameDir 'bin\x64\Sunrise\logs\sunrise.log'

# The mod keeps the log open, so read it with a sharing-tolerant reader.
function Read-LogLines {
    $stream = [IO.File]::Open($log, 'Open', 'Read', 'ReadWrite')
    try {
        $reader = New-Object IO.StreamReader($stream)
        try { return $reader.ReadToEnd() -split "`r?`n" } finally { $reader.Dispose() }
    } finally { $stream.Dispose() }
}

# The pose fields of the newest `ev=vr.xr hand` line, without the timestamp, so two samples can
# be compared for equality.
function Hand-Fields {
    $hit = (Read-LogLines) | Select-String 'ev=vr\.xr hand' | Select-Object -Last 1
    if (-not $hit) { return $null }
    return ($hit.Line -replace '^.*ev=vr\.xr hand ', '')
}

# Applies one mock input state and waits for the module to actually publish it.
#
# Waiting a fixed time was not enough. The mock only re-reads its input file when the file's
# last-write time changes, which it notices within about a second -- but a sample taken on a fixed
# sleep can still land on a line logged just before the re-read, and then reports the PREVIOUS
# step's state. That produced two false failures that looked exactly like a broken axis. Waiting
# for the published fields to differ from the previous step's is the real condition.
function Get-HandSample {
    param([hashtable]$Mock, [string]$Label)
    $previous = Hand-Fields
    $countBefore = ((Read-LogLines) | Select-String 'ev=vr\.xr hand').Count
    Set-MockInput @Mock | Out-Null
    # Settled means either the published fields changed, or enough fresh lines have gone by that
    # the mock must have re-read and simply had nothing to change -- which is the case whenever a
    # step asks for a state the mock is already in, the first one especially.
    $deadline = (Get-Date).AddSeconds($SettleSeconds + 8)
    do {
        Start-Sleep -Milliseconds 400
        $fields = Hand-Fields
        $fresh = ((Read-LogLines) | Select-String 'ev=vr\.xr hand').Count - $countBefore
    } while ($fields -eq $previous -and $fresh -lt 5 -and (Get-Date) -lt $deadline)
    if ($fields -eq $previous -and $fresh -lt 5) { throw "no fresh hand lines at all after '$Label'" }
    # One more line, so the value read is settled rather than the first frame of the transition.
    Start-Sleep -Milliseconds 1200
    $line = (Read-LogLines) | Select-String 'ev=vr\.xr hand' | Select-Object -Last 1 | ForEach-Object { $_.Line }
    $sample = @{ Label = $Label; Line = $line }
    foreach ($field in 'r_valid', 'l_valid') {
        if ($line -match "$field=(\d)") { $sample[$field] = [int]$Matches[1] }
    }
    # The module folds the engine's body yaw into every published vector, and in orbit that yaw is
    # whatever the camera happens to be pointing at. The frame's own fold angle is logged with the
    # poses, so undo it here and compare against values worked out at yaw zero.
    $yaw = 0.0
    if ($line -match 'yaw=(-?[\d.]+)') { $yaw = [double]$Matches[1] }
    $sample['yaw'] = $yaw
    $sin = [Math]::Sin(-$yaw); $cos = [Math]::Cos(-$yaw)
    foreach ($field in 'r_fwd', 'r_up', 'r_off', 'l_fwd', 'l_off', 'h_off') {
        if ($line -match "$field=(-?[\d.]+),(-?[\d.]+),(-?[\d.]+)") {
            $x = [double]$Matches[1]; $y = [double]$Matches[2]; $z = [double]$Matches[3]
            $sample[$field] = @(($x * $cos - $y * $sin), ($x * $sin + $y * $cos), $z)
        }
    }
    return $sample
}

$failures = New-Object System.Collections.Generic.List[string]
$checks = 0

# Compares one parsed vector with an expected triple.
function Assert-Vector {
    param([hashtable]$Sample, [string]$Field, [double[]]$Expected, [string]$What)
    $script:checks++
    $actual = $Sample[$Field]
    if (-not $actual) { $failures.Add("$($Sample.Label): $Field missing"); return }
    for ($i = 0; $i -lt 3; $i++) {
        if ([Math]::Abs($actual[$i] - $Expected[$i]) -gt $Tolerance) {
            $failures.Add(("{0}: {1} {2} = {3} expected {4}" -f $Sample.Label, $Field, $What,
                ($actual -join ','), ($Expected -join ',')))
            return
        }
    }
    Write-Output ("  OK   {0,-22} {1} = {2}" -f $Sample.Label, $Field, ($actual -join ','))
}

# Every field is written explicitly on every call: the mock keeps the previous value for anything
# omitted, so a partial write would silently carry state from the previous step into this one.
$zero = @{ HeadYaw = 0; HeadPitch = 0; HeadX = 0; HeadY = 0; HeadZ = 0
    HandYaw = 0; HandPitch = 0; HandX = 0; HandY = 0; HandZ = 0
    LHandYaw = 0; LHandPitch = 0; LHandX = 0; LHandY = 0; LHandZ = 0 }
function With { param([hashtable]$Changes) $m = $zero.Clone(); foreach ($k in $Changes.Keys) { $m[$k] = $Changes[$k] }; return $m }

if (-not (Get-GameProcess)) { throw 'game is not running; run Run-Orbit.ps1 first' }
[void](Focus-Game)
Write-Output ('client rect: ' + (Export-ClientRect))
if (-not ((Read-LogLines) | Select-String 'ev=vr\.xr init result=ok')) {
    Write-Output 'no session in the log; pressing F9'
    Send-GameKey F9
    Start-Sleep -Seconds 8
}

Write-Output '== rest pose: hand 0.3 forward, 0.2 right, 0.2 below the eyes =='
$rest = Get-HandSample (With @{}) 'rest'
Write-Output ("  raw: " + $rest.Line)
if ($rest.r_valid -ne 1) { $failures.Add('rest: right hand not valid -- no action space, or untracked') }
if ($rest.l_valid -ne 1) { $failures.Add('rest: left hand not valid') }
Assert-Vector $rest 'r_off' @(0.30, -0.20, -0.20) 'offset at rest'
Assert-Vector $rest 'r_fwd' @(1, 0, 0) 'forward at rest (game X forward)'
Assert-Vector $rest 'r_up'  @(0, 0, 1) 'up at rest (game Z up)'
# The mock mirrors the left hand on X only, so it sits 0.2 LEFT, i.e. game +Y.
Assert-Vector $rest 'l_off' @(0.30, 0.20, -0.20) 'left offset at rest'

Write-Output '== rotations: +HandYaw is a left turn about the game up axis =='
# +30 deg about OpenXR +Y turns -Z toward -X, i.e. left, which in the game is +Y.
$yaw = Get-HandSample (With @{ HandYaw = 30 }) 'HandYaw +30'
Assert-Vector $yaw 'r_fwd' @(0.866, 0.5, 0) 'forward yawed left 30'
Assert-Vector $yaw 'r_up'  @(0, 0, 1) 'up unchanged by yaw'
Assert-Vector $yaw 'r_off' @(0.30, -0.20, -0.20) 'offset unchanged by yaw'

$yawNeg = Get-HandSample (With @{ HandYaw = -30 }) 'HandYaw -30'
Assert-Vector $yawNeg 'r_fwd' @(0.866, -0.5, 0) 'forward yawed right 30'

# A right-handed +30 deg about OpenXR +X takes -Z toward +Y, so the aim goes UP, and the up vector
# tips backward, which in the game is -X.
$pitch = Get-HandSample (With @{ HandPitch = 30 }) 'HandPitch +30'
Assert-Vector $pitch 'r_fwd' @(0.866, 0, 0.5) 'forward pitched up 30'
Assert-Vector $pitch 'r_up'  @(-0.5, 0, 0.866) 'up tipped back 30'

$pitchNeg = Get-HandSample (With @{ HandPitch = -30 }) 'HandPitch -30'
Assert-Vector $pitchNeg 'r_fwd' @(0.866, 0, -0.5) 'forward pitched down 30'

Write-Output '== translations: one axis at a time, 0.3 m =='
$handX = Get-HandSample (With @{ HandX = 0.3 }) 'HandX +0.3'
Assert-Vector $handX 'r_off' @(0.30, -0.50, -0.20) '0.3 m right is -0.3 on game Y'
$handY = Get-HandSample (With @{ HandY = 0.3 }) 'HandY +0.3'
Assert-Vector $handY 'r_off' @(0.30, -0.20, 0.10) '0.3 m up is +0.3 on game Z'
$handZ = Get-HandSample (With @{ HandZ = 0.3 }) 'HandZ +0.3'
Assert-Vector $handZ 'r_off' @(0.00, -0.20, -0.20) '0.3 m backward is -0.3 on game X'

Write-Output '== the hands are independent =='
$lhand = Get-HandSample (With @{ LHandYaw = 40 }) 'LHandYaw +40'
Assert-Vector $lhand 'l_fwd' @(0.766, 0.643, 0) 'left forward yawed left 40'
Assert-Vector $lhand 'r_fwd' @(1, 0, 0) 'right forward untouched by the left hand'

Write-Output '== the head moves, the hand does not =='
# This is the decoupling the whole phase rests on, checked in numbers before it is checked in
# pixels: both offsets are measured from the same recentre origin, so the head may travel while
# the hand stays exactly where it was. HeadZ -0.5 is a lean forward (OpenXR -Z is forward), HeadX
# +0.3 a lean to the right, which the game reads as +0.5 on X and -0.3 on Y.
$headMoved = Get-HandSample (With @{ HeadZ = -0.5; HeadX = 0.3 }) 'head leans in'
Assert-Vector $headMoved 'r_off' @(0.30, -0.20, -0.20) 'hand offset unchanged by head travel'
Assert-Vector $headMoved 'h_off' @(0.50, -0.30, 0.00) 'head offset follows the head'

Write-Output '== the hand moves, the head does not =='
$handMoved = Get-HandSample (With @{ HandZ = -0.4; HandY = 0.25 }) 'hand reaches out'
Assert-Vector $handMoved 'r_off' @(0.70, -0.20, 0.05) 'hand offset follows the hand'
Assert-Vector $handMoved 'h_off' @(0.00, 0.00, 0.00) 'head offset unchanged by hand travel'

Set-MockInput @zero | Out-Null
Write-Output ''
if ($failures.Count -eq 0) {
    Write-Output "PASS  $checks/$checks checks"
} else {
    Write-Output "FAIL  $($failures.Count) of $checks checks"
    $failures | ForEach-Object { Write-Output "  $_" }
    exit 1
}
