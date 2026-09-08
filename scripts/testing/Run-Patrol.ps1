# Full cycle into a patrol zone: launch on the mock, character select, orbit, one Director launch,
# and let the forced destination (SVR_Destination.txt, read by hooks/vr/vr_destination.cpp) decide
# where the ship lands. Works in windowed mode: every click is expressed in 1920x1080 "design"
# coordinates and mapped onto the live client rectangle.
param(
    [string]$Destination = 'eden_freeroam 20 160 7',
    [switch]$NoToggle,
    [int]$SettleSeconds = 70,
    [int]$LoadSeconds = 85,
    [int]$ArriveSeconds = 100
)
$ErrorActionPreference = 'Stop'
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'lib\GameIO.ps1')
$shots = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'build\shots')
if (-not (Test-Path $shots)) { New-Item -ItemType Directory -Force -Path $shots | Out-Null }
$log = 'C:\Games\Sunrise\bin\x64\Sunrise\logs\sunrise.log'

Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices;
public struct SVRRECT { public int L, T, R, B; }
public struct SVRPOINT { public int X, Y; }
public static class SVRWin {
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out SVRRECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref SVRPOINT p);
}
'@

# Maps a 1920x1080 design coordinate onto the game's client area, in physical pixels.
function Get-ClientMap {
    $p = Get-GameProcess
    $h = $p.MainWindowHandle
    $r = New-Object SVRRECT
    [void][SVRWin]::GetClientRect($h, [ref]$r)
    $o = New-Object SVRPOINT
    [void][SVRWin]::ClientToScreen($h, [ref]$o)
    return @{ X = $o.X; Y = $o.Y; W = $r.R - $r.L; H = $r.B - $r.T }
}
function Map-Point([hashtable]$m, [int]$X, [int]$Y) {
    return @([int]($m.X + $X * $m.W / 1920.0), [int]($m.Y + $Y * $m.H / 1080.0))
}
# The Director needs real pointer motion before a node shows its card.
function Hover-Click([hashtable]$m, [int]$X, [int]$Y, [double]$Settle = 1.2) {
    $pt = Map-Point $m $X $Y
    [void][SunriseVR.Native]::SetCursorPos($pt[0] - 9, $pt[1] - 5)
    Start-Sleep -Milliseconds 300
    [SunriseVR.Native]::mouse_event(0x0001, 5, 3, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 300
    [SunriseVR.Native]::mouse_event(0x0001, 4, 2, 0, [IntPtr]::Zero)
    Start-Sleep -Seconds $Settle
    [SunriseVR.Native]::mouse_event(0x0002, 0, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 80
    [SunriseVR.Native]::mouse_event(0x0004, 0, 0, 0, [IntPtr]::Zero)
}
function Log-Grep([string]$Pattern, [int]$Last = 6) {
    Select-String -Path $log -Pattern $Pattern | Select-Object -Last $Last |
        ForEach-Object { $_.Line.Substring(0, [Math]::Min(200, $_.Line.Length)) }
}

Clear-MockInput
Stop-Game
Remove-Item C:\Games\Sunrise\SVR_MockXR.log, C:\Games\Sunrise\SVR_Weapon.txt -ErrorAction SilentlyContinue
[System.IO.File]::WriteAllText('C:\Games\Sunrise\SVR_Destination.txt', "$Destination`n", (New-Object System.Text.UTF8Encoding($false)))
Remove-Item C:\Games\Sunrise\SVR_Destination.off -ErrorAction SilentlyContinue
$manifest = Enable-MockXr
Write-Output "mock manifest: $manifest"
Write-Output "destination: $Destination"
Start-Game -Fresh | Out-Null
$p = Wait-GameWindow -TimeoutSeconds 240
if (-not $p) { throw 'no game window' }
Start-Sleep -Seconds $SettleSeconds
[void](Focus-Game)
Send-GameKey RETURN
Start-Sleep -Seconds 40
[void](Focus-Game)
$m = Get-ClientMap
Write-Output ("client: {0},{1} {2}x{3}" -f $m.X, $m.Y, $m.W, $m.H)
Save-Shot (Join-Path $shots 'p_charsel.png') | Out-Null
$pt = Map-Point $m 1360 455
Send-GameClick -X $pt[0] -Y $pt[1]
Start-Sleep -Seconds $LoadSeconds
[void](Focus-Game)
Save-Shot (Join-Path $shots 'p_orbit.png') | Out-Null
Write-Output ('orbit: ' + (Get-GameStats))
Write-Output '---- forced destination'
Log-Grep 'ev=vr\.dest'

# Any launch will do; the Tower path is the one with known coordinates.
$m = Get-ClientMap
Hover-Click $m 960 848 2.0;   Start-Sleep 5      # OPEN DIRECTOR
Save-Shot (Join-Path $shots 'p_director.png') | Out-Null
Hover-Click $m 960 250;       Start-Sleep 6      # Tower
Hover-Click $m 908 290;       Start-Sleep 3      # landing node
Save-Shot (Join-Path $shots 'p_launch_panel.png') | Out-Null
Hover-Click $m 1587 884;      Start-Sleep 5      # LAUNCH
Save-Shot (Join-Path $shots 'p_launching.png') | Out-Null
Start-Sleep -Seconds $ArriveSeconds
[void](Focus-Game)
Save-Shot (Join-Path $shots 'p_arrived.png') | Out-Null
Write-Output ('arrived: ' + (Get-GameStats))
Write-Output '---- landing'
Log-Grep 'svc=42 stage=configuration|changed world to|stage=region result=(forced|public)|Entering state .activity:in_world|ev=vr\.dest' 12

if (-not $NoToggle) {
    Send-GameKey F9
    Start-Sleep -Seconds 6
    Save-Shot (Join-Path $shots 'p_xr_a.png') | Out-Null
    Write-Output ('after F9: ' + (Get-GameStats))
    Log-Grep 'ev=vr\.(xr init|probe pose)' 4
}
