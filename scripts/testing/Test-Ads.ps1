# Physical ADS, measured, in a running session (a first-person area, F9 on, a weapon in hand).
#
# What it checks, in order:
#   learn     the weapon in hand has a complete profile (its hip and ADS poses); taught here with
#             `force on` if it has not, so the rest has a learned sight to work with.
#   align     the controller is moved so the learned sight line passes through the eye the image is
#             rendered from (`ev=vr.aim line` sight_r), head level and straight, controller level.
#   enter     aligned            -> the detector holds ADS, and the ENGINE goes into ADS (its zoom).
#   band      3 cm off, from ADS -> stays (inside the exit radius, 6 cm).
#   out       7 cm off           -> leaves.
#   gap       5 cm off, from hip -> does not enter (between the radii: the hysteresis band).
#   back      3 cm off, from hip -> enters.
#   gaze_in   head turned 15 deg -> stays (exit angle 20).
#   gaze_out  head turned 25 deg -> leaves.
#   gaze_no   head turned 15 deg, from hip -> does not enter (enter angle 12).
#   slip      `trace` of an ADS cycle with the compensation on and the controller STILL (ADS held by
#             `force`): the drawn GUN relative to the controller must not move -- its worst single
#             frame under 6 mm and 0.6 deg (ads_trace.py "visible slip (GUN ...)"). The controller is
#             kept still because the drawn pose reaches the GPU a frame after the controller is read,
#             so a moving controller shows its own motion as "slip"; and the gun, not the hand,
#             because the game re-grips the hand by ~4 deg in ADS, which is the hand's own business.
#   zoom      every enter/exit logged zoom_check=ok: the world was rendered at the VR FOV, not zoomed.
#   converge  the fired ray of a shot taken aligned meets the sight line at the zero distance.
#
# Head and hand stay level so the camera's axes are the room's; offsets are applied along the head's
# up axis, which is the mock's HandY.
#
# -TwoHand runs the whole matrix with the left hand holding the foregrip (GripL, placed on the
# foregrip the arm module learned, `twohand dump`), carried rigidly with the right hand at every
# step: the hold must not disturb the sight, the detector, the compensation or the zeroed shot.
# Wants a long gun in hand (fc_len >= 0.18).
param(
    [string]$Out = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'build\shots\adstest'),
    [double]$Settle = 0.8,
    [switch]$TwoHand
)
$ErrorActionPreference = 'Stop'
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'lib\GameIO.ps1')
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'lib\VrCommands.ps1')
if (-not (Test-Path $Out)) { New-Item -ItemType Directory -Force -Path $Out | Out-Null }
$inv = [System.Globalization.CultureInfo]::InvariantCulture
$traceTool = Join-Path $PSScriptRoot 'ads_trace.py'
$results = New-Object System.Collections.ArrayList

function Get-Vec([string]$text, [string]$name) {
    if ($text -notmatch "$name=([-\d.]+),([-\d.]+),([-\d.]+)") { return $null }
    return @([double]::Parse($Matches[1], $inv), [double]::Parse($Matches[2], $inv), [double]::Parse($Matches[3], $inv))
}
function Get-Num([string]$text, [string]$name) {
    if ($text -notmatch "$name=([-\d.]+)") { return [double]::NaN }
    return [double]::Parse($Matches[1], $inv)
}
function Dot($a, $b) { return $a[0] * $b[0] + $a[1] * $b[1] + $a[2] * $b[2] }
# Fresh = logged after the request. By timestamp, not by counting matches: a count taken over the
# log's last N lines stops growing once old matches scroll out of the window, and then hands back a
# stale line (it read a seat 1.08 deg "moved" that had not moved at all).
function Get-LogT([string]$line) { if ($line -match ' t=(\d+) ') { return [long]$Matches[1] } return -1 }
function Get-Fresh([string]$pattern, [scriptblock]$trigger, [int]$timeoutMs = 4000) {
    $since = (Read-VrLog 400 | ForEach-Object { Get-LogT $_ } | Measure-Object -Maximum).Maximum
    & $trigger
    $deadline = (Get-Date).AddMilliseconds($timeoutMs)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 150
        $lines = @(Read-VrLog 4000 | Where-Object { $_ -match $pattern -and (Get-LogT $_) -gt $since })
        if ($lines.Count -gt 0) { return $lines[-1] }
    }
    return $null
}
function Get-AimLine { Get-Fresh 'ev=vr\.aim line' { [void](Send-Aim @('dump')) } }
function Get-AdsState { Get-Fresh 'ev=vr\.ads state=' { [void](Send-Ads @('dump')) } }
function Record([string]$step, [bool]$ok, [string]$detail) {
    [void]$results.Add([pscustomobject]@{ step = $step; verdict = $(if ($ok) { 'OK' } else { 'FAIL' }); detail = $detail })
    Write-Host ("{0,-10} {1,-5} {2}" -f $step, $(if ($ok) { 'OK' } else { 'FAIL' }), $detail)
}

# The left hand: at rest, or on the foregrip relative to wherever the right hand is (right controller
# level and straight, so its frame is the head's; same mapping as Test-TwoHand's Left-On).
$script:fc = $null
function With-Left([hashtable]$p) {
    $q = @{} + $p
    if ($script:fc) {
        $q['LHandX'] = [double]$q['HandX'] + 0.4 - $script:fc[1]
        $q['LHandY'] = [double]$q['HandY'] + $script:fc[2]
        $q['LHandZ'] = [double]$q['HandZ'] - $script:fc[0]
        $q['GripL'] = 1
    }
    return $q
}
function Set-Pose([hashtable]$p) { $q = With-Left $p; [void](Set-MockInput @q) }
function Get-Hold {
    $line = Get-Fresh 'ev=vr\.arms hold state=' { [void](Send-Arms @('twohand dump')) }
    $h = @{}
    foreach ($m in [regex]::Matches("$line", '(\w+)=([^\s]+)')) { $h[$m.Groups[1].Value] = $m.Groups[2].Value }
    return $h
}

[void](Focus-Game)
[void](Send-Ads @('mode auto', 'force off', 'comp on', 'learn on'))
[void](Send-Aim @('ray off', 'sight on'))
$pose = @{ HandX = 0.0; HandY = 0.10; HandZ = -0.25; HandYaw = 0; HandPitch = 0 }
Set-Pose $pose
Start-Sleep -Seconds 2.5
if ($TwoHand) {
    $h = Get-Hold
    if ($h['fc_ok'] -ne '1' -or [double]::Parse("$($h['fc_len'])", $inv) -lt 0.18) { throw "no long-gun foregrip known: fc_ok=$($h['fc_ok']) fc_len=$($h['fc_len'])" }
    $script:fc = @($h['fc'] -split ',' | ForEach-Object { [double]::Parse($_, $inv) })
    Set-Pose $pose
    Start-Sleep -Seconds 1.5
    $h = Get-Hold
    Record 'hold' ($h['state'] -eq 'held') ("state={0} line_err={1} fc={2}" -f $h['state'], $h['line_err'], $h['fc'])
}

# --- learn
$state = Get-AdsState
if ("$state" -notmatch 'complete=1') {
    Write-Host 'teaching the weapon in hand: ADS held 3 s'
    [void](Send-Ads @('force on'))
    Start-Sleep -Seconds 3
    [void](Send-Ads @('force off'))
    Start-Sleep -Seconds 2.5
    $state = Get-AdsState
}
Record 'learn' ("$state" -match 'complete=1') ("profile=" + (Get-Num "$state" 'profile') + " hip_fov=" + (Get-Num "$state" 'hip_fov'))

# --- align: move the controller so the sight line passes through the eye
$sightR = [double]::NaN
for ($i = 0; $i -lt 4; $i++) {
    $line = Get-AimLine
    $o = Get-Vec "$line" 'sight_o'
    $dd = Get-Vec "$line" 'sight_d'
    $f = Get-Vec "$line" 'cam_fwd'
    $u = Get-Vec "$line" 'cam_up'
    if (-not $o -or -not $dd -or -not $f -or -not $u) { break }
    $l = @(($u[1] * $f[2] - $u[2] * $f[1]), ($u[2] * $f[0] - $u[0] * $f[2]), ($u[0] * $f[1] - $u[1] * $f[0]))
    $t = Dot $o $dd
    $p = @(($o[0] - $dd[0] * $t), ($o[1] - $dd[1] * $t), ($o[2] - $dd[2] * $t))
    $sightR = [Math]::Sqrt((Dot $p $p))
    if ($sightR -lt 0.002) { break }
    # Move the hand by -p, expressed in the head's axes (mock: X right, Y up, Z back).
    $pose['HandX'] = [double]$pose['HandX'] + (Dot $p $l)
    $pose['HandY'] = [double]$pose['HandY'] - (Dot $p $u)
    $pose['HandZ'] = [double]$pose['HandZ'] + (Dot $p $f)
    Set-Pose $pose
    Start-Sleep -Milliseconds 700
}
$aligned = @{} + $pose
Record 'align' ($sightR -lt 0.004) ("sight_r={0:F4} m hand={1:F3},{2:F3},{3:F3}" -f $sightR, $pose.HandX, $pose.HandY, $pose.HandZ)

function Pose-At([double]$up = 0, [double]$headYaw = 0) {
    $p = @{} + $aligned
    $p['HandY'] = [double]$p['HandY'] + $up
    $p['HeadYaw'] = $headYaw
    return $p
}
function Go([double]$up = 0, [double]$headYaw = 0) {
    $p = Pose-At $up $headYaw
    Set-Pose $p
}
function Check([string]$step, [hashtable]$p, [bool]$expectHeld) {
    Set-Pose $p
    Start-Sleep -Seconds $Settle
    $s = Get-AdsState
    $held = "$s" -match 'state=(held|leaving)'
    $want = "$s" -match 'want=1'
    # The engine itself in ADS: its FOV narrowed (zoom above 1), a little after the button.
    $zoom = Get-Num "$s" 'zoom'
    $ok = ($held -eq $expectHeld) -and ($want -eq $expectHeld) -and ($expectHeld -eq ($zoom -gt 1.02))
    Record $step $ok ("expect={0} state={1} r={2} gaze_deg={3} engine_zoom={4:F3}" -f $(if ($expectHeld) { 'ads' } else { 'hip' }),
        $(if ("$s" -match 'state=(\w+)') { $Matches[1] } else { '?' }), (Get-Num "$s" ' r'), (Get-Num "$s" 'gaze_deg'), $zoom)
    Save-Shot (Join-Path $Out "$step.png") | Out-Null
}

$enterBefore = @(Read-VrLog 4000 | Where-Object { $_ -match 'ev=vr\.ads (enter|exit) ' }).Count
# Start from the hip, so `enter` is a real entry (and its haptic tick can be counted).
Go 0.07
Start-Sleep -Seconds 1.2
$hapticsBefore = @(Read-VrLog 20000 | Where-Object { $_ -match 'ev=vr\.xr haptic hand=R' }).Count
Check 'enter'    (Pose-At 0) $true
$hapticsAfter = @(Read-VrLog 20000 | Where-Object { $_ -match 'ev=vr\.xr haptic hand=R' }).Count
Record 'haptic' (($hapticsAfter - $hapticsBefore) -eq 1) ("right-hand pulses on entering: {0}" -f ($hapticsAfter - $hapticsBefore))
Check 'band'     (Pose-At 0.03) $true
Check 'out'      (Pose-At 0.07) $false
Check 'gap'      (Pose-At 0.05) $false
Check 'back'     (Pose-At 0.03) $true
Check 'gaze_in'  (Pose-At 0 15) $true
Check 'gaze_out' (Pose-At 0 25) $false
Check 'gaze_no'  (Pose-At 0 15) $false

# --- slip: an ADS cycle traced with the compensation on, controller still, ADS held by `force`
Go 0.07
Start-Sleep -Seconds 1.5
[void](Send-Ads @('trace 6'))
Start-Sleep -Milliseconds 700
[void](Send-Ads @('force on'))
Start-Sleep -Seconds 2.5
[void](Send-Ads @('force off'))
Start-Sleep -Seconds 4
$csv = Join-Path $Out 'slip_trace.csv'
Copy-Item C:\Games\Sunrise\SVR_AdsTrace.csv $csv -Force
$report = & python $traceTool $csv 2>$null
$report | ForEach-Object { Write-Host "  $_" }
$slip = "$report" -match 'visible slip \(GUN vs controller\).*?: ([\d.]+) mm at t=[\d.]+ ms, ([\d.]+) deg'
$slipMm = if ($slip) { [double]::Parse($Matches[1], $inv) } else { [double]::NaN }
$slipDeg = if ($slip) { [double]::Parse($Matches[2], $inv) } else { [double]::NaN }
Record 'slip' ($slipMm -lt 6.0 -and $slipDeg -lt 0.6) ("{0} mm, {1} deg" -f $slipMm, $slipDeg)

# --- zoom: did anything ever reach the image?
$events = @(Read-VrLog 4000 | Where-Object { $_ -match 'ev=vr\.ads (enter|exit) ' })
$events = if ($events.Count -gt $enterBefore) { $events[$enterBefore..($events.Count - 1)] } else { @() }
$bad = @($events | Where-Object { $_ -notmatch 'zoom_check=ok' })
Record 'zoom' ($events.Count -gt 0 -and $bad.Count -eq 0) ("{0} enter/exit events, {1} not ok" -f $events.Count, $bad.Count)

# --- converge: fire once aligned, and find where the fired ray meets the sight line
Go 0
Start-Sleep -Seconds 1.2
$line = Get-AimLine
$serial0 = if ("$(Get-VrLog 'ev=vr\.aim shot ' 1)" -match 'serial=(\d+)') { [int]$Matches[1] } else { 0 }
[SunriseVR.Native]::mouse_event(0x0002, 0, 0, 0, [IntPtr]::Zero)
Start-Sleep -Milliseconds 60
[SunriseVR.Native]::mouse_event(0x0004, 0, 0, 0, [IntPtr]::Zero)
Start-Sleep -Milliseconds 800
$shot = Get-VrLog 'ev=vr\.aim shot ' 1
$serial = if ("$shot" -match 'serial=(\d+)') { [int]$Matches[1] } else { 0 }
$pos = Get-Vec "$shot" 'new_pos'
$fwd = Get-Vec "$shot" 'new_fwd'
$eye = Get-Vec "$shot" 'eye'
$so = Get-Vec "$line" 'sight_o'
$sd = Get-Vec "$line" 'sight_d'
$lo = Get-Vec "$line" 'origin'
if ($TwoHand) {
    $h = Get-Hold
    Record 'held_end' ($h['state'] -eq 'held') ("state={0} line_err={1} hand_err={2}" -f $h['state'], $h['line_err'], $h['hand_err'])
}
if ($serial -gt $serial0 -and $pos -and $fwd -and $so -and $sd -and $lo -and ("$shot" -match 'sighted=1')) {
    # Camera-relative: the shot starts at the line's origin (the muzzle) along fwd; closest approach
    # between the two lines, and how far down the sight line it happens.
    $w0 = @(($lo[0] - $so[0]), ($lo[1] - $so[1]), ($lo[2] - $so[2]))
    $a = Dot $fwd $fwd; $b = Dot $fwd $sd; $c = Dot $sd $sd; $dw = Dot $fwd $w0; $e = Dot $sd $w0
    $den = $a * $c - $b * $b
    $sc = ($b * $e - $c * $dw) / $den
    $tc = ($a * $e - $b * $dw) / $den
    $gap = @((($w0[0] + $fwd[0] * $sc) - $sd[0] * $tc), (($w0[1] + $fwd[1] * $sc) - $sd[1] * $tc), (($w0[2] + $fwd[2] * $sc) - $sd[2] * $tc))
    $gapM = [Math]::Sqrt((Dot $gap $gap))
    Record 'converge' ($gapM -lt 0.01 -and [Math]::Abs($tc - 25) -lt 1.0) ("rays meet {0:F2} m down the sight line, {1:F4} m apart (zero 25 m)" -f $tc, $gapM)
} else {
    Record 'converge' $false ("no sighted shot logged (serial {0} -> {1}): {2}" -f $serial0, $serial, "$shot")
}
Go 0.07

Write-Host ''
$results | Format-Table -AutoSize | Out-String -Width 220 | Write-Host
$fails = @($results | Where-Object { $_.verdict -ne 'OK' }).Count
Write-Host ("{0}/{1} OK" -f ($results.Count - $fails), $results.Count)
