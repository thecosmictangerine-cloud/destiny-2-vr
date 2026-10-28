# HANDOFF-2026-10-18 #1: swapping the LEFT hand's weapon in the inventory crashed (destiny2.exe+0xCB4949, the
# old weapon object called after the engine replaced it). On a session in the world, VR on, Hunter layout:
#  1. dual wield with the energy weapon (slot 1) in the left hand, calibrated, fired once
#  2. the inventory opened (VR off for the mouse), the energy weapon swapped for the first other one
#  3. back in the world: the left trigger pulled -- the game alive, the left hand waiting until the HUD names
#     the new object (`left_wait reason=unconfirmed`), the old object forgotten, then the new one fired
# Results in build\shots\invswap\.

param([string]$Out = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'build\shots\invswap'))

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
$rest = @{ HandX = 0.12; HandY = 0.15; HandZ = -0.1; LHandX = -0.12; LHandY = 0.15; LHandZ = -0.1 }
Assert-GameFocus -Quiet | Out-Null
Set-MockInput @rest | Out-Null
Send-GameKey '1'; Start-Sleep 3

# 1. Dual wield, the energy weapon in the left hand.
$t0 = Log-Now
Send-Cmd 'SVR_Dual.txt' @('calibrate on', 'left 1', 'dual on')
$rec = Wait-LogSince $t0 'ev=vr\.dual calibrate (recorded|timeout)' 15
Start-Sleep 2
$t1 = Log-Now
Set-MockInput @rest -TrigL 1 | Out-Null; Start-Sleep -Milliseconds 600; Set-MockInput @rest | Out-Null
$burst = Wait-LogSince $t1 'ev=vr\.dual left_burst' 5
# A weapon recorded earlier in the session needs no calibration: then no line comes, and the shot proves it.
$recorded = if ($null -ne $rec) { $rec -match 'recorded' } else { $null -ne $burst }
Check 'left weapon calibrated' $recorded $(if ($null -ne $rec) { "$rec" } else { 'recorded earlier in the session' })
Check 'left hand fires before the swap' ($null -ne $burst) "$burst"

# 2. The inventory: VR off for the mouse, the energy slot, the first other weapon of the flyout.
Send-GameKey F9; Start-Sleep 3
Send-GameKey F1; Start-Sleep 4
Send-GameClick -X 519 -Y 535; Start-Sleep 2
Save-Shot (Join-Path $Out 'flyout.png') | Out-Null
Send-GameClick -X 208 -Y 535; Start-Sleep 3
Save-Shot (Join-Path $Out 'swapped.png') | Out-Null
Send-GameKey ESCAPE; Start-Sleep 3
Send-GameKey F9; Start-Sleep 5

# 3. Back in the world: pull the left trigger right away and keep pulling.
$t2 = Log-Now
for ($i = 0; $i -lt 6; $i++) {
    Set-MockInput @rest -TrigL 1 | Out-Null; Start-Sleep -Milliseconds 400; Set-MockInput @rest | Out-Null; Start-Sleep -Milliseconds 600
}
Start-Sleep 4
$alive = Get-GameProcess
Check 'the game survives the swap' ($null -ne $alive) (Get-GameStats)
$slot = @(Log-Since $t1 'ev=vr\.dual slot=1 weapon=')
Check 'the new energy weapon is taken for slot 1' ($slot.Count -gt 0) ($slot -join ' | ')
$forget = @(Log-Since $t1 'ev=vr\.offhand forget key=')
Check 'the old object is forgotten' ($forget.Count -gt 0) ($forget -join ' | ')
$t3 = Log-Now
Start-Sleep 8
Set-MockInput @rest -TrigL 1 | Out-Null; Start-Sleep -Milliseconds 600; Set-MockInput @rest | Out-Null
$after = Wait-LogSince $t3 'ev=vr\.dual left_burst' 8
Check 'the new left weapon fires once recorded' ($null -ne $after) "$after"
Send-Cmd 'SVR_Dual.txt' @('dual off')
$results | Export-Csv (Join-Path $Out 'results.csv') -NoTypeInformation
$pass = @($results | Where-Object Verdict -eq 'PASS').Count
"SUMMARY $pass/$($results.Count) passed -> $Out"
