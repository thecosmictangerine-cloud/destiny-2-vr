# Dual wield on the mock, in a running session (a first-person area, F9 on, the loadout's weapon in the
# right hand being slot 0). docs/DUAL-WIELD.md section 4.
#
# What it checks, in order:
#   loadout     the three loadout weapons are known, each with a magazine and a rate of fire read from
#               the weapon's own stats.
#   calibrate   asking for a left weapon never held shows it in the right hand for a moment, records it
#               and gives the right hand its weapon back.
#   left_fire   the left trigger, held one second: the left weapon fires at its own rate (rounds within
#               15 % of seconds / interval), every round launched for the LEFT weapon leaves rewritten,
#               from left of the eye, along the left controller (the mock's controllers point where the
#               head does); its magazine drops by the rounds fired and the right weapon's does not.
#   right_fire  the right trigger meanwhile is the game's own and still leaves from the right.
#   reload      the left weapon emptied: it reloads by itself, with the engine's transfer, back to full.
#   twin        the right hand's weapon in the left hand too: both fire, each round from its own hand.
#   launcher    a third weapon, a projectile one, in the left hand.
#   holster     the left hand at the left hip, grip squeezed: dual wield on with the energy weapon; again:
#               put away.
#
# Screenshots of each pair in build/shots/dualtest.
param(
    [string]$Out = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'build\shots\dualtest')
)
$ErrorActionPreference = 'Stop'
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'lib\GameIO.ps1')
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'lib\VrCommands.ps1')
if (-not (Test-Path $Out)) { New-Item -ItemType Directory -Force -Path $Out | Out-Null }
$inv = [System.Globalization.CultureInfo]::InvariantCulture
$results = New-Object System.Collections.ArrayList

function Dual([string[]]$l) { [void](Send-VrFile 'SVR_Dual.txt' $l) }
function Get-LogT([string]$line) { if ($line -match ' t=(\d+) ') { return [long]$Matches[1] } return -1 }
function Now-T { (Read-VrLog 300 | ForEach-Object { Get-LogT $_ } | Measure-Object -Maximum).Maximum }
function Since([long]$t0, [string]$pattern) {
    @(Read-VrLog 8000 | Where-Object { $_ -match $pattern -and (Get-LogT $_) -gt $t0 })
}
function Fields([string]$line) {
    $h = @{}
    foreach ($m in [regex]::Matches("$line", '(\w+)=([^\s]+)')) { $h[$m.Groups[1].Value] = $m.Groups[2].Value }
    return $h
}
function Vec([string]$v) { return @($v -split ',' | ForEach-Object { [double]::Parse($_, $inv) }) }
# Logged directions carry four decimals: normalised before any angle is taken, or two prints of the
# same direction read 0.8 deg apart (acos(0.9999)).
function Unit($a) { $n = [Math]::Sqrt($a[0] * $a[0] + $a[1] * $a[1] + $a[2] * $a[2]); return @(($a[0] / $n), ($a[1] / $n), ($a[2] / $n)) }
function AngleDeg($a, $b) {
    $u = Unit $a; $v = Unit $b
    return [Math]::Acos([Math]::Max(-1.0, [Math]::Min(1.0, $u[0] * $v[0] + $u[1] * $v[1] + $u[2] * $v[2]))) * 57.2958
}
function Num($h, [string]$k) { if ($h.ContainsKey($k)) { return [double]::Parse("$($h[$k])", $inv) } return [double]::NaN }
function Record([string]$step, [bool]$ok, [string]$detail) {
    [void]$results.Add([pscustomobject]@{ step = $step; ok = $ok; detail = $detail })
    Write-Host ("{0,-11} {1,-5} {2}" -f $step, $(if ($ok) { 'OK' } else { 'FAIL' }), $detail)
}
function Weapons {
    $t0 = Now-T
    Dual @('dump')
    Start-Sleep -Milliseconds 900
    $w = @{}
    foreach ($l in (Since $t0 'ev=vr\.dual weapon slot=')) { $f = Fields $l; $w[[int]$f['slot']] = $f }
    return $w
}
function State {
    $t0 = Now-T
    Dual @('dump')
    Start-Sleep -Milliseconds 900
    return Fields (@(Since $t0 'ev=vr\.dual state ')[-1])
}

$rest = @{ HandX = 0.12; HandY = 0.0; HandZ = 0.0; LHandX = -0.12; LHandY = 0.0; LHandZ = 0.0 }
function Pose([hashtable]$extra) {
    $q = @{} + $rest
    if ($extra) { foreach ($k in $extra.Keys) { $q[$k] = $extra[$k] } }
    [void](Set-MockInput @q)
}

Assert-GameFocus | Out-Null
[void](Send-VrFile 'SVR_Display.txt' @('shape desktop'))
Dual @('dual off', 'calibrate on', 'kick on', 'counter on', 'holsters on')
Pose
Send-GameKey '1'
Start-Sleep 3
Clear-WeaponIdle | Out-Null
Start-Sleep 2

# --- loadout
$w = Weapons
$okLoadout = $w.Count -eq 3
$detail = ''
foreach ($s in 0..2) {
    $f = $w[$s]
    if (-not $f) { $okLoadout = $false; continue }
    $i = Num $f 'interval_stat'
    if (-not ($i -gt 0.02 -and $i -lt 5) -or (Num $f 'capacity') -le 0) { $okLoadout = $false }
    $detail += ("s{0}: mag {1}/{2} interval {3:N4}s; " -f $s, $f['magazine'], $f['capacity'], $i)
}
Record 'loadout' $okLoadout $detail
$rightKey = $w[0]['key']

# --- calibrate: the left hand asks for slot 1 (already recorded earlier in the session: nothing to do)
$wasRecorded = $w[1]['recorded'] -eq '1'
$t0 = Now-T
Dual @('left 1', 'dual on')
$deadline = (Get-Date).AddSeconds(12)
$done = $null
while ((Get-Date) -lt $deadline -and -not $done -and -not $wasRecorded) {
    Start-Sleep -Milliseconds 400
    $done = @(Since $t0 'ev=vr\.dual calibrate (recorded|timeout)')[0]
}
if ($wasRecorded) { $done = 'ev=vr.dual calibrate recorded (earlier in the session)' }
Start-Sleep 3.5
$w = Weapons
Record 'calibrate' ("$done" -match 'recorded' -and $w[1]['recorded'] -eq '1' -and $w[0]['active'] -eq '1') `
    ("{0}; slot1 recorded={1}; right hand back on slot 0: active={2}" -f $(if ($done) { ($done -split 'ev=')[1] } else { 'no calibration' }), $w[1]['recorded'], $w[0]['active'])
Save-Shot (Join-Path $Out '01_right_s0_left_s1.png') | Out-Null

function Burst([double]$seconds, [string]$label) {
    $before = Weapons
    $t0 = Now-T
    Pose @{ TrigL = 1 }
    Start-Sleep -Milliseconds ([int]($seconds * 350))
    Save-Shot (Join-Path $Out ("{0}_firing.png" -f $label)) | Out-Null
    Start-Sleep -Milliseconds ([int]($seconds * 650))
    Pose
    Start-Sleep 1.2
    $after = Weapons
    $burst = @(Since $t0 'ev=vr\.dual left_burst ')
    $shots = @(Since $t0 'ev=vr\.aim shot ')
    return @{ before = $before; after = $after; burst = $burst; shots = $shots }
}

# --- left fire
$leftKey = $w[1]['key']
$r = Burst 1.0 '02_left_rifle'
$b = Fields "$($r.burst[-1])"
$interval = Num $r.after[1] 'interval_stat'
$n = [int](Num $b 'shots')
$expected = 1 + [Math]::Floor((Num $b 'seconds') / $interval + 0.25)
$leftShots = @($r.shots | Where-Object { $_ -match "weapon=$leftKey" })
$bad = 0
$worstDeg = 0.0
$spreadDeg = 0.0
$minLeft = 99.0
# The reference is the left shot the module computed (`shot_fwd`, logged with the burst) and the left
# controller's pointing direction (`ctrl_fwd`). Not the character's aim: the left weapon's own recoil
# climbs the character's pitch during a burst (as the right weapon's does), which the VR view and the
# rewritten rounds both ignore.
$ref = Vec $b['shot_fwd']
$ctrl = Vec $b['ctrl_fwd']
$aimDeg = AngleDeg $ref $ctrl
$first = $null
foreach ($l in $leftShots) {
    $f = Fields $l
    if ($f['rewritten'] -ne '1') { $bad++; continue }
    $eye = Vec $f['eye']; $pos = Vec $f['new_pos']; $nf = Vec $f['new_fwd']; $ef = Vec $f['engine_fwd']
    if (-not $first) { $first = $nf }
    $worstDeg = [Math]::Max($worstDeg, (AngleDeg $nf $ref))
    $spreadDeg = [Math]::Max($spreadDeg, (AngleDeg $nf $first))
    # Left of the eye: the game's +Y is left, so left of forward is (-f.y, f.x).
    $d = @(($pos[0] - $eye[0]), ($pos[1] - $eye[1]))
    $lat = $d[0] * (-$ef[1]) + $d[1] * $ef[0]
    $minLeft = [Math]::Min($minLeft, $lat)
}
$magDrop = [int](Num $r.before[1] 'magazine') - [int](Num $r.after[1] 'magazine')
$rightDrop = [int](Num $r.before[0] 'magazine') - [int](Num $r.after[0] 'magazine')
Record 'left_rate' ($n -ge 2 -and [Math]::Abs($n - $expected) -le [Math]::Max(1, 0.15 * $expected) -and [Math]::Abs((Num $b 'interval') - $interval) -le 0.2 * $interval + 0.017) `
    ("{0} rounds in {1}s, expected {2}; mean interval {3} against the weapon's {4:N4}" -f $n, $b['seconds'], $expected, $b['interval'], $interval)
Record 'left_rounds' ($leftShots.Count -ge 1 -and $bad -eq 0 -and $worstDeg -lt 0.1 -and $spreadDeg -lt 0.1 -and $aimDeg -lt 1.5 -and $minLeft -gt 0.05) `
    ("{0} launches of the left weapon, {1} not rewritten; worst {2:N3} deg off the left shot, {3:N3} deg between rounds; the shot {4:N2} deg off where the left controller points; nearest {5:N3} m left of the eye" -f $leftShots.Count, $bad, $worstDeg, $spreadDeg, $aimDeg, $minLeft)
Record 'left_ammo' ($magDrop -eq $n -and $rightDrop -eq 0) ("left magazine -{0} for {1} rounds; right magazine -{2}" -f $magDrop, $n, $rightDrop)

# --- right fire, the game's own
$t0 = Now-T
Clear-WeaponIdle -SettleMs 600 | Out-Null
$rs = @(Since $t0 "ev=vr\.aim shot .*weapon=$rightKey")
$okRight = $false; $det = 'no shot of the right weapon'
if ($rs.Count -gt 0) {
    $f = Fields $rs[-1]
    $eye = Vec $f['eye']; $pos = Vec $f['new_pos']; $ef = Vec $f['engine_fwd']
    $lat = ($pos[0] - $eye[0]) * (-$ef[1]) + ($pos[1] - $eye[1]) * $ef[0]
    $okRight = $f['rewritten'] -eq '1' -and $lat -lt -0.05
    $det = ("rewritten={0}, {1:N3} m right of the eye" -f $f['rewritten'], -$lat)
}
Record 'right_fire' $okRight $det

# --- reload: empty the left weapon
$cap = [int](Num $w[1] 'capacity')
$t0 = Now-T
Pose @{ TrigL = 1 }
# Held until the magazine runs dry and the reload starts, then let go.
$deadline = (Get-Date).AddSeconds([Math]::Min(10.0, $cap * $interval + 2.0))
while ((Get-Date) -lt $deadline -and @(Since $t0 'ev=vr\.dual left_reload start').Count -eq 0) { Start-Sleep -Milliseconds 150 }
Pose
Start-Sleep -Milliseconds 400
Save-Shot (Join-Path $Out '03_left_reloading.png') | Out-Null
Start-Sleep 2.2
$start = @(Since $t0 'ev=vr\.dual left_reload start')
$end = @(Since $t0 'ev=vr\.dual left_reload done')
$w = Weapons
Record 'reload' ($start.Count -ge 1 -and $end.Count -ge 1 -and [int](Num $w[1] 'magazine') -eq $cap) `
    ("{0} started, {1} done, magazine {2}/{3}" -f $start.Count, $end.Count, $w[1]['magazine'], $cap)

# --- twin: the right hand's weapon in the left as well
Dual @('left 0')
Start-Sleep 1.5
Save-Shot (Join-Path $Out '04_twin.png') | Out-Null
$r = Burst 0.8 '05_twin'
$twinShots = @($r.shots | Where-Object { $_ -match "weapon=$rightKey" })
$b = Fields "$($r.burst[-1])"
$leftOnes = 0
foreach ($l in $twinShots) {
    $f = Fields $l
    $eye = Vec $f['eye']; $pos = Vec $f['new_pos']; $ef = Vec $f['engine_fwd']
    if ((($pos[0] - $eye[0]) * (-$ef[1]) + ($pos[1] - $eye[1]) * $ef[0]) -gt 0.05 -and $f['rewritten'] -eq '1') { $leftOnes++ }
}
$drop = [int](Num $r.before[0] 'magazine') - [int](Num $r.after[0] 'magazine')
Record 'twin' ($b['twin'] -eq '1' -and [int](Num $b 'shots') -ge 1 -and $leftOnes -ge 1 -and $drop -eq [int](Num $b 'shots')) `
    ("{0} rounds from the left of the same weapon ({1} logged from the left), its one magazine -{2}" -f $b['shots'], $leftOnes, $drop)

# --- the launcher in the left hand
$glRecorded = $w[2]['recorded'] -eq '1'
$t0 = Now-T
Dual @('left 2')
$deadline = (Get-Date).AddSeconds(12)
$done = $null
while ((Get-Date) -lt $deadline -and -not $done -and -not $glRecorded) {
    Start-Sleep -Milliseconds 400
    $done = @(Since $t0 'ev=vr\.dual calibrate (recorded|timeout)')[0]
}
Start-Sleep 3.5
Save-Shot (Join-Path $Out '06_right_s0_left_s2.png') | Out-Null
$w = Weapons
$glKey = $w[2]['key']
$r = Burst 0.5 '07_left_launcher'
$gl = @($r.shots | Where-Object { $_ -match "weapon=$glKey" -and $_ -match 'rewritten=1' })
Record 'launcher' ($w[2]['recorded'] -eq '1' -and $gl.Count -ge 1) ("recorded={0}, {1} launcher rounds rewritten to the left" -f $w[2]['recorded'], $gl.Count)

# --- holsters: the left hand at the left hip (energy, slot 1)
Dual @('dual off')
Start-Sleep 1
$hip = @{ LHandX = -0.04; LHandY = -0.39; LHandZ = 0.18 }
Pose $hip
Start-Sleep 0.8
$t0 = Now-T
Pose ($hip + @{ GripL = 1 }); Start-Sleep 0.6
Pose $hip; Start-Sleep 0.6
$s = State
$draw = @(Since $t0 'ev=vr\.dual holster hand=left action=draw slot=1')
$okDraw = $draw.Count -ge 1 -and $s['dual'] -eq '1' -and $s['left_slot'] -eq '1'
$t0 = Now-T
Pose ($hip + @{ GripL = 1 }); Start-Sleep 0.6
Pose $hip; Start-Sleep 0.6
$s2 = State
$put = @(Since $t0 'ev=vr\.dual holster hand=left action=put_away')
Record 'holster' ($okDraw -and $put.Count -ge 1 -and $s2['dual'] -eq '0') ("draw: dual={0} slot={1}; put away: dual={2}" -f $s['dual'], $s['left_slot'], $s2['dual'])
Pose

$failed = @($results | Where-Object { -not $_.ok })
Write-Output ("{0} checks, {1} failed" -f $results.Count, $failed.Count)
$results | Export-Csv -NoTypeInformation -Path (Join-Path $Out 'results.csv')
