# Life after a sparrow ride (first headset session, 2026-10-18; docs/HANDOFF-SWORD-SPARROW.md): getting off
# gave the Guardian a new physics component while Sunrise's tracker kept the old, bodiless one, and from then
# on a sword was ridden like a sparrow, a super stayed in third person and a second ride never became a
# cockpit. On the mock, in a session already in the world, VR on, a sword in the power slot:
#  1. ride (X held), throttle, get off (A)
#  2. swap primary and secondary, fire, back to the primary -- what the player did
#  3. the sword: on_foot, the eye at the head -- never `vehicle`
#  4. a second ride: vehicle, physics found, it moves
#  5. off again, the sword again: on_foot
# `own_player found` in the log says the stale component was met and the stand-in took over.
# Results in build\shots\sparrow2\.

param([string]$Out = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'build\shots\sparrow2'))

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\lib\GameIO.ps1')
. (Join-Path $PSScriptRoot '..\lib\VrSession.ps1')
New-Item -ItemType Directory -Force $Out | Out-Null
$results = New-Object System.Collections.ArrayList
function Check([string]$Name, [bool]$Ok, [string]$Detail) {
    $v = if ($Ok) { 'PASS' } else { 'FAIL' }
    [void]$results.Add([pscustomobject]@{ Check = $Name; Verdict = $v; Detail = $Detail })
    '{0} {1,-46} {2}' -f $v, $Name, $Detail
}
function Fields([string]$Line) {
    $h = @{}
    foreach ($m in [regex]::Matches($Line, '(\w+)=([^\s]+)')) { $h[$m.Groups[1].Value] = $m.Groups[2].Value }
    return $h
}
function Num([string]$s) { return [double]::Parse(($s -split ',')[0], [Globalization.CultureInfo]::InvariantCulture) }

$rest = @{ HandX = 0.05; HandY = 0.0; HandZ = -0.05; LHandX = -0.05; LHandY = 0.0; LHandZ = -0.05 }

function Ride([string]$Tag) {
    $t0 = Log-Now
    Set-MockInput @rest -BtnX 1 | Out-Null; Start-Sleep -Milliseconds 1200; Set-MockInput @rest | Out-Null
    $m = Wait-LogSince $t0 'ev=vr\.mode mode=vehicle' 12
    Check "$Tag -> mode vehicle" ($null -ne $m) "$m"
    Start-Sleep 4
    Set-MockInput @rest -TrigR 1 | Out-Null; Start-Sleep -Milliseconds 1500
    $tA = Log-Now; Start-Sleep -Milliseconds 700
    $r = @(Log-Since $tA 'ev=vr\.mode ride ' | Select-Object -Last 1)
    Compose-Eye "${Tag}_ride" $Out | Out-Null
    Set-MockInput @rest | Out-Null
    $speed = if ($r.Count -gt 0) { Num (Fields $r[0])['speed'] } else { 0 }
    Check "$Tag moves" ($speed -gt 4) ("speed={0}" -f $speed)
    Start-Sleep 2
    $tF = Log-Now
    Set-MockInput @rest -BtnA 1 | Out-Null; Start-Sleep -Milliseconds 300; Set-MockInput @rest | Out-Null
    $off = Wait-LogSince $tF 'ev=vr\.mode mode=first was=(vehicle|on_foot|passive)' 10
    Check "$Tag off -> first person" ($null -ne $off) "$off"
    Start-Sleep 2
}

function Sword([string]$Tag) {
    $t0 = Log-Now
    Send-GameKey '3'
    $m = Wait-LogSince $t0 'ev=vr\.mode mode=(on_foot|vehicle|passive) was=first' 8
    Check "$Tag sword -> on_foot" ($null -ne $m -and $m -match 'mode=on_foot') "$m"
    Start-Sleep 3
    $wrong = @(Log-Since $t0 'ev=vr\.mode mode=vehicle')
    Check "$Tag sword never ridden" ($wrong.Count -eq 0) $(if ($wrong.Count) { $wrong[0] } else { 'no vehicle mode' })
    Compose-Eye "${Tag}_sword" $Out | Out-Null
    $tB = Log-Now
    Send-GameKey '1'
    $back = Wait-LogSince $tB 'ev=vr\.mode mode=first' 8
    Check "$Tag gun again -> first" ($null -ne $back) "$back"
    Start-Sleep 2
}

$tStart = Log-Now
Assert-GameFocus -Quiet | Out-Null
Set-MockInput @rest | Out-Null
Send-GameKey '1'; Start-Sleep 3

# 1. The first ride.
Ride 'ride1'

# 2. What the player did next: primary and secondary swapped, fired, back.
Send-GameKey '2'; Start-Sleep 2
Set-MockInput @rest -TrigR 1 | Out-Null; Start-Sleep -Milliseconds 800; Set-MockInput @rest | Out-Null
Start-Sleep 1
Send-GameKey '1'; Start-Sleep 2
Set-MockInput @rest -TrigR 1 | Out-Null; Start-Sleep -Milliseconds 800; Set-MockInput @rest | Out-Null
Start-Sleep 3
$reacq = @(Log-Since $tStart 'ev=vr\.mode vehicle (reacquired|found)' | Where-Object { $_ -match 'reacquired' })
$own = @(Log-Since $tStart 'ev=vr\.mode own_player found')
"info: own_player found x$($own.Count) $(if ($own.Count) { $own[0] })"

# 3. The sword after a ride.
Sword 'after1'

# 4. A second ride.
Ride 'ride2'

# 5. The sword again.
Sword 'after2'

$results | Export-Csv (Join-Path $Out 'results.csv') -NoTypeInformation
$pass = @($results | Where-Object Verdict -eq 'PASS').Count
"SUMMARY $pass/$($results.Count) passed -> $Out"
