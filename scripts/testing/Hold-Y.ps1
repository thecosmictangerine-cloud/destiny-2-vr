# Holds Y on the mock (dual wield on or off) in a running first-person session and reports whether
# the game survived, with the dual-wield log lines since. The 2026-10-17 crash's reproduction.
#
# The session MUST have had VR from launch (Test-Lifecycle.ps1 -SkipEvents gets there): with F9
# pressed after landing the loadout was learned in the world and the crash never showed.
#   -VrOn       press F9 first     -DrawFirst  show slot 1 in the right hand first (the A/B)
# SVR_DualTrace.txt in the game dir says which engine call came last if it dies.
param([switch]$VrOn, [switch]$DrawFirst, [int]$WatchSeconds = 12)
$ErrorActionPreference = 'Stop'
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'lib\GameIO.ps1')
$log = 'C:\Games\Sunrise\bin\x64\Sunrise\logs\sunrise.log'
function Tail-From([long]$from) {
    $fs = [IO.File]::Open($log, 'Open', 'Read', 'ReadWrite')
    [void]$fs.Seek($from, 'Begin')
    $t = (New-Object IO.StreamReader($fs)).ReadToEnd(); $fs.Close(); return $t
}
[void](Focus-GameHard)
Set-MockInput | Out-Null
if ($VrOn) { Send-GameKey F9; Start-Sleep -Seconds 8 }
if ($DrawFirst) {
    Send-GameKey '2'; Start-Sleep -Seconds 4
    Send-GameKey '1'; Start-Sleep -Seconds 4
}
$from = (Get-Item $log).Length
Set-MockInput -BtnY 1 | Out-Null
Start-Sleep -Milliseconds 1200
Set-MockInput | Out-Null
Start-Sleep -Seconds $WatchSeconds
$alive = [bool](Get-Process destiny2 -ErrorAction SilentlyContinue)
Write-Output "alive: $alive"
(Tail-From $from) -split "`n" | Where-Object { $_ -match 'ev=vr\.dual' } | ForEach-Object { $_.Substring(0, [Math]::Min(200, $_.Length)) }
