# The sparrow as a cockpit (docs/SWORD-SPARROW.md), on the mock, in a session already in the world.
#
#  1. summoned from the controllers (X held 0.8 s = the game's Tab, then E held): the mode goes vehicle, the vehicle's physics found, the handlebars learned
#  2. the eye sits on the seat, not behind it: the composition shows the handlebars and the nose
#  3. throttle on the right trigger: it moves, the eye rides with it
#  4. the horizon stays level whatever the vehicle's pitch (the written up vector against world up)
#  5. the head looks freely: a 60-degree head turn while riding does not steer (the heading holds)
#  6. the stick steers: the heading turns, the room anchor turns with it
#  7. the handlebars: grip both, push the right one forward, it turns left
#  8. dismount (A): on foot, then first person
# Results in build\shots\sparrow\.

param([string]$Out = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'build\shots\sparrow'))

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\lib\GameIO.ps1')
. (Join-Path $PSScriptRoot '..\lib\VrSession.ps1')
New-Item -ItemType Directory -Force $Out | Out-Null
$results = New-Object System.Collections.ArrayList
function Check([string]$Name, [bool]$Ok, [string]$Detail) {
    $v = if ($Ok) { 'PASS' } else { 'FAIL' }
    [void]$results.Add([pscustomobject]@{ Check = $Name; Verdict = $v; Detail = $Detail })
    '{0} {1,-44} {2}' -f $v, $Name, $Detail
}
function Fields([string]$Line) {
    $h = @{}
    foreach ($m in [regex]::Matches($Line, '(\w+)=([^\s]+)')) { $h[$m.Groups[1].Value] = $m.Groups[2].Value }
    return $h
}
function Num([string]$s) { return [double]::Parse(($s -split ',')[0], [Globalization.CultureInfo]::InvariantCulture) }
function Ride-Yaw([int64]$After) {
    $l = @(Log-Since $After 'ev=vr\.mode ride ' | Select-Object -Last 1)
    if ($l.Count -eq 0) { return $null }
    return Fields $l[0]
}
# The heading's total turn over the ride lines logged in (t0, t1], wrapped step by step (degrees, + left).
function Turn-Between([int64]$T0, [int64]$T1) {
    $lines = @(Log-Since $T0 'ev=vr\.mode ride ' | Where-Object { (Get-LineTime $_) -le $T1 })
    $sum = 0.0; $prev = $null
    foreach ($l in $lines) { $y = Num (Fields $l)['yaw']; if ($null -ne $prev) { $sum += (Yaw-Diff $y $prev) }; $prev = $y }
    return $sum * 57.2958
}
function Yaw-Diff([double]$a, [double]$b) { $d = $a - $b; while ($d -gt [Math]::PI) { $d -= 2 * [Math]::PI }; while ($d -lt -[Math]::PI) { $d += 2 * [Math]::PI }; return $d }

$rest = @{ HandX = 0.05; HandY = 0.0; HandZ = -0.05; LHandX = -0.05; LHandY = 0.0; LHandZ = -0.05 }
Assert-GameFocus -Quiet | Out-Null
Set-MockInput @rest | Out-Null
Send-GameKey '1'; Start-Sleep 3

# 1. Summon.
$t0 = Log-Now
# From the controllers: X held 0.8 s plays the game's summon (Tab, then E held).
Set-MockInput @rest -BtnX 1 | Out-Null; Start-Sleep -Milliseconds 1200; Set-MockInput @rest | Out-Null
$summon = Wait-LogSince $t0 'ev=vr\.pad summon=sparrow' 4
Check 'X held summons the sparrow' ($null -ne $summon) "$summon"
$m = Wait-LogSince $t0 'ev=vr\.mode mode=vehicle' 12
Check 'summoned -> mode vehicle' ($null -ne $m) "$m"
$found = Wait-LogSince $t0 'ev=vr\.mode vehicle found' 5
Check 'vehicle physics found' ($null -ne $found) "$found"
Start-Sleep 12
$state = @(Log-Since $t0 'ev=vr\.mode state' | Select-Object -Last 1)
Check 'handlebars learned from the rider' ($state.Count -gt 0 -and $state[0] -match 'bars=1') "$($state[0])"
Compose-Eye 'sp_rest' $Out | Out-Null

# 3. Throttle (short bursts: the mock rides straight on, into whatever is ahead).
Set-MockInput @rest -TrigR 1 | Out-Null; Start-Sleep -Milliseconds 1500
$tA = Log-Now; Start-Sleep -Milliseconds 700
$r1 = Ride-Yaw $tA
Compose-Eye 'sp_ride' $Out | Out-Null
Set-MockInput @rest | Out-Null
Check 'throttle moves it' ($null -ne $r1 -and (Num $r1['speed']) -gt 4) ("speed={0}" -f $(if ($r1) { $r1['speed'] } else { 'none' }))
Start-Sleep 2

# 5. The head looks freely: a head turn injects nothing (the engine camera's yaw is what steers).
$tB = Log-Now; Start-Sleep -Milliseconds 700
$e0 = Ride-Yaw $tB
Set-MockInput @rest -HeadYaw 60 | Out-Null; Start-Sleep 2
$tC = Log-Now; Start-Sleep -Milliseconds 700
$e1 = Ride-Yaw $tC
Compose-Eye 'sp_look' $Out | Out-Null
if ($e0 -and $e1) {
    $drift = [Math]::Abs((Yaw-Diff (Num $e1['engine_yaw']) (Num $e0['engine_yaw']))) * 57.2958
    $vdrift = [Math]::Abs((Yaw-Diff (Num $e1['yaw']) (Num $e0['yaw']))) * 57.2958
    Check 'head turn does not steer' ($drift -lt 5 -and $vdrift -lt 5) ("engine yaw {0:N2} deg, heading {1:N2} deg, head 60 deg left" -f $drift, $vdrift)
} else { Check 'head turn does not steer' $false 'no ride lines' }
Set-MockInput @rest | Out-Null; Start-Sleep 1

# 6. The stick steers.
$tD = Log-Now
Set-MockInput @rest -TrigR 1 -MoveX 1 | Out-Null; Start-Sleep -Milliseconds 1300
Set-MockInput @rest | Out-Null
$tE = Log-Now; Start-Sleep -Milliseconds 800
$turn = Turn-Between $tD ($tE + 400)
Check 'stick right turns right' ($turn -lt -15) ("heading {0:N1} deg over the 1.3 s of stick" -f $turn)
Start-Sleep 2

# 7. The handlebars: the hands where the rider's are (left/right of the seat, low and forward).
# The rider's grips sit 7 cm ahead of the eye, 33 cm to each side, 35 cm below it; the mock's hands rest at
# (+-0.2, 1.4, -0.3) with the grip 7 cm behind and 3 cm under the aim.
$bars = $rest.Clone(); $bars['HandX'] = 0.13; $bars['HandY'] = -0.12; $bars['HandZ'] = 0.16; $bars['LHandX'] = -0.13; $bars['LHandY'] = -0.12; $bars['LHandZ'] = 0.16
Set-MockInput @bars -GripL 1 -GripR 1 | Out-Null; Start-Sleep 1
Compose-Eye 'sp_bars' $Out | Out-Null
$grab = @(Log-Since ($tD) 'ev=vr\.body bar grab')
Check 'both hands take the handlebars' ($grab.Count -ge 2) ($grab -join ' | ')
$tF = Log-Now
$push = $bars.Clone(); $push['HandZ'] = 0.04
Set-MockInput @push -TrigR 1 -GripL 1 -GripR 1 | Out-Null; Start-Sleep -Milliseconds 1300
Set-MockInput @bars -GripL 1 -GripR 1 | Out-Null
$tG = Log-Now; Start-Sleep -Milliseconds 800
$barTurn = Turn-Between $tF ($tG + 400)
Check 'right bar forward turns left' ($barTurn -gt 10) ("heading {0:N1} deg over the 1.3 s push" -f $barTurn)

# 8. Dismount.
Set-MockInput @rest | Out-Null; Start-Sleep 2
$tF = Log-Now
Set-MockInput @rest -BtnA 1 | Out-Null; Start-Sleep -Milliseconds 300; Set-MockInput @rest | Out-Null
$off = Wait-LogSince $tF 'ev=vr\.mode mode=(on_foot|first) was=vehicle' 8
Check 'A dismounts' ($null -ne $off) "$off"

$results | Export-Csv (Join-Path $Out 'results.csv') -NoTypeInformation
$pass = @($results | Where-Object Verdict -eq 'PASS').Count
"SUMMARY $pass/$($results.Count) passed -> $Out"
