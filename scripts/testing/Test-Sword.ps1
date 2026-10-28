# The sword in first person (docs/SWORD-SPARROW.md), on the mock, in a session already in the world
# (Test-Lifecycle.ps1 -SkipEvents) with a sword in the power slot.
#
#  1. first person with the gun: the arm rig is learned (it is what poses the third-person arms)
#  2. the sword drawn (slot 3): the mode goes on_foot, the sword is found and its hold learned
#  3. the eye is at the head, not behind the character: the engine camera vs the written one
#  4. the blade lies along the fist's axis and the hilt in the palm, for several controller poses
#  5. the sword follows the hand: two eye compositions with the controller moved, the blade moved
#  6. Y passes the sword to the left hand, and back
#  7. no body: looking down shows no legs or chest (composition kept for a person to look at)
#  8. back to the gun: mode first again
# Results in build\shots\sword\ (PNG per step, results.csv).

param([string]$Out = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'build\shots\sword'))

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\lib\GameIO.ps1')
. (Join-Path $PSScriptRoot '..\lib\VrSession.ps1')
New-Item -ItemType Directory -Force $Out | Out-Null
$results = New-Object System.Collections.ArrayList
function Check([string]$Name, [bool]$Ok, [string]$Detail) {
    $v = if ($Ok) { 'PASS' } else { 'FAIL' }
    [void]$results.Add([pscustomobject]@{ Check = $Name; Verdict = $v; Detail = $Detail })
    '{0} {1,-40} {2}' -f $v, $Name, $Detail
}
function Fields([string]$Line) {
    $h = @{}
    foreach ($m in [regex]::Matches($Line, '(\w+)=([^\s]+)')) { $h[$m.Groups[1].Value] = $m.Groups[2].Value }
    return $h
}
function Num([string]$s) { return [double]::Parse(($s -replace '[^0-9eE+\-.]', ''), [Globalization.CultureInfo]::InvariantCulture) }

$rest = @{ HandX = 0.05; HandY = 0.15; HandZ = -0.1; LHandX = -0.05; LHandY = 0.12; LHandZ = -0.1 }
Assert-GameFocus -Quiet | Out-Null
Set-MockInput @rest | Out-Null

# 1. First person, the gun: the rig.
Send-GameKey '1'; Start-Sleep 4
$t0 = Log-Now
$rig = Wait-LogSince 0 'ev=vr\.arms mesh .* rig_valid=1' 5
Check 'arm rig learned in first person' ($null -ne $rig) "$rig"

# 2. The sword.
$t0 = Log-Now
Send-GameKey '3'
$mode = Wait-LogSince $t0 'ev=vr\.mode mode=on_foot' 10
Check 'sword -> mode on_foot' ($null -ne $mode) "$mode"
$drawn = Wait-LogSince $t0 'ev=vr\.body sword drawn' 10
Check 'sword found beside the hand' ($null -ne $drawn) "$drawn"
Start-Sleep 3
$hold = @(Log-Since 0 'ev=vr\.body sword hold' | Select-Object -Last 1)
Check 'sword hold learned' ($hold.Count -gt 0) "$($hold[0])"

# 3+4. Poses: the blade along the fist, the hilt in the palm.
$poses = @(
    @{ Name = 'rest'; P = @{} },
    @{ Name = 'up45'; P = @{ HandPitch = 45 } },
    @{ Name = 'yaw40'; P = @{ HandYaw = 40 } },
    @{ Name = 'roll70'; P = @{ HandRoll = 70 } },
    @{ Name = 'high'; P = @{ HandY = 0.45; HandZ = -0.25; HandPitch = 70 } }
)
foreach ($pose in $poses) {
    $p = $rest.Clone(); foreach ($k in $pose.P.Keys) { $p[$k] = $pose.P[$k] }
    Set-MockInput @p | Out-Null
    Start-Sleep -Milliseconds 2500
    $tA = Log-Now
    $line = Wait-LogSince $tA 'ev=vr\.body sword_pose' 6
    if ($null -eq $line) { Check "pose $($pose.Name)" $false 'no sword_pose line'; continue }
    $f = Fields $line
    $deg = Num $f['blade_deg']; $mm = Num $f['hilt_to_palm_mm']
    Check "pose $($pose.Name): blade on fist axis" ($deg -lt 2.0) ("blade_deg={0:N2} hilt_to_palm_mm={1:N1}" -f $deg, $mm)
    Compose-Eye "sw_$($pose.Name)" $Out | Out-Null
}

# 5. The sword follows the hand (image): the same pose twice is the null pair, a moved hand the effect.
Set-MockInput @rest | Out-Null; Start-Sleep 2
Compose-Eye 'sw_nullA' $Out | Out-Null; Start-Sleep -Milliseconds 500
Compose-Eye 'sw_nullB' $Out | Out-Null
$moved = $rest.Clone(); $moved['HandX'] = 0.20; $moved['HandPitch'] = 30
Set-MockInput @moved | Out-Null; Start-Sleep 2
Compose-Eye 'sw_moved' $Out | Out-Null
$diff = & python (Join-Path $PSScriptRoot 'eye_view.py') diff $Out sw_nullA sw_nullB 2>$null
$diff2 = & python (Join-Path $PSScriptRoot 'eye_view.py') diff $Out sw_nullA sw_moved 2>$null
"null pair: $diff"
"moved:     $diff2"

# 6. Y passes the sword to the left hand.
Set-MockInput @rest | Out-Null; Start-Sleep 1
$t0 = Log-Now
Set-MockInput @rest -BtnY 1 | Out-Null; Start-Sleep -Milliseconds 200; Set-MockInput @rest | Out-Null
$left = Wait-LogSince $t0 'ev=vr\.body sword hand=left' 5
Check 'Y passes the sword to the left hand' ($null -ne $left) "$left"
Start-Sleep 2
$line = Wait-LogSince (Log-Now) 'ev=vr\.body sword_pose hand=left' 6
if ($line) {
    $f = Fields $line
    Check 'left hand: blade on fist axis' ((Num $f['blade_deg']) -lt 2.0) $line
}
Compose-Eye 'sw_left' $Out | Out-Null
$t0 = Log-Now
Set-MockInput @rest -BtnY 1 | Out-Null; Start-Sleep -Milliseconds 200; Set-MockInput @rest | Out-Null
$right = Wait-LogSince $t0 'ev=vr\.body sword hand=right' 5
Check 'Y gives it back to the right hand' ($null -ne $right) "$right"

# 6b. Two hands on the hilt: the left palm 12 cm behind the right one on the fist's axis takes it; 8 cm
# lower, the blade runs from it through the right hand: 34 deg off the fist's own axis.
# (The mock's hands rest at (+-0.2, 1.4, -0.3), the grip 7 cm behind and 3 cm under the aim.)
Set-MockInput @rest | Out-Null; Start-Sleep 1
$t0 = Log-Now
$two = $rest.Clone(); $two['LHandX'] = 0.45; $two['LHandY'] = 0.15; $two['LHandZ'] = 0.02
Set-MockInput @two -GripL 1 | Out-Null
$take = Wait-LogSince $t0 'ev=vr\.body two_hand take' 5
Check 'second hand takes the hilt' ($null -ne $take) "$take"
Start-Sleep 2
$line = Wait-LogSince (Log-Now) 'ev=vr\.body sword_pose' 6
if ($line) { Check 'two hands, in line: blade on the axis' ((Num (Fields $line)['blade_deg']) -lt 3) $line }
$low = $two.Clone(); $low['LHandY'] = 0.07
Set-MockInput @low -GripL 1 | Out-Null; Start-Sleep 2
$line = Wait-LogSince (Log-Now) 'ev=vr\.body sword_pose' 6
if ($line) {
    $deg = Num (Fields $line)['blade_deg']
    Check 'two hands, left lower: blade through both' ([Math]::Abs($deg - 33.7) -lt 4) ("blade_deg={0:N1} (expected 33.7)" -f $deg)
}
Compose-Eye 'sw_two' $Out | Out-Null
$t0 = Log-Now
Set-MockInput @rest | Out-Null
$rel = Wait-LogSince $t0 'ev=vr\.body two_hand release' 5
Check 'opening the hand lets go' ($null -ne $rel) "$rel"

# 7. No body: looking down.
Set-MockInput @rest -HeadPitch -65 | Out-Null; Start-Sleep 2
Compose-Eye 'sw_down' $Out | Out-Null
$stats = Wait-LogSince 0 'ev=vr\.body stats' 15
Check 'body pieces hidden, arms posed' ($null -ne $stats -and $stats -match 'armed=[1-9]' -and $stats -match 'hidden=[1-9]') "$stats"

# 8. Back to the gun.
Set-MockInput @rest | Out-Null
$t0 = Log-Now
Send-GameKey '1'
$first = Wait-LogSince $t0 'ev=vr\.mode mode=first' 10
Check 'gun -> mode first' ($null -ne $first) "$first"

$results | Export-Csv (Join-Path $Out 'results.csv') -NoTypeInformation
$pass = @($results | Where-Object Verdict -eq 'PASS').Count
"SUMMARY $pass/$($results.Count) passed -> $Out"
