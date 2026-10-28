# Reproduces the destination-load hang on the mock: boot, character select, orbit, then VR switched
# on (or on and off, or never) BEFORE a Director launch, and a verdict from the log: did the game
# reach `activity:in_world` and keep running, or did Sunrise's stall probe fire?
#
# Evidence that motivated it (2026-10-17): across every log on file, 5 of 5 loads hung when F9 had
# been on earlier in the session and 0 of 13 when it had never been pressed.
#
#   -F9 none     never switch VR on                       (control)
#   -F9 on       switch VR on in orbit, travel with it on
#   -F9 onoff    switch VR on in orbit, then off, then travel
#   -Loads n     travel n times (orbit -> zone -> orbit -> zone ...), stopping at the first hang
#   -Prep        lines written to SVR_Debug.txt before launch (bisection switches)
param(
    [ValidateSet('none', 'on', 'onoff')][string]$F9 = 'on',
    [string]$Destination = 'eden_freeroam 20 160 7',
    [int]$Loads = 1,
    [string[]]$Prep = @(),
    [ValidateSet('desktop', 'eye')][string]$Shape = 'desktop',
    [string]$Tag = 'repro',
    [int]$SettleSeconds = 70,
    [int]$LoadSeconds = 85,
    [int]$WorldTimeoutSeconds = 150,
    [int]$AfterWorldSeconds = 25
)
$ErrorActionPreference = 'Stop'
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'lib\GameIO.ps1')
$shots = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'build\shots')
if (-not (Test-Path $shots)) { New-Item -ItemType Directory -Force -Path $shots | Out-Null }
$log = 'C:\Games\Sunrise\bin\x64\Sunrise\logs\sunrise.log'

Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices;
public struct SVRRECT2 { public int L, T, R, B; }
public struct SVRPOINT2 { public int X, Y; }
public static class SVRWin2 {
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out SVRRECT2 r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref SVRPOINT2 p);
}
'@

function Get-ClientMap {
    $p = Get-GameProcess
    $h = $p.MainWindowHandle
    $r = New-Object SVRRECT2
    [void][SVRWin2]::GetClientRect($h, [ref]$r)
    $o = New-Object SVRPOINT2
    [void][SVRWin2]::ClientToScreen($h, [ref]$o)
    return @{ X = $o.X; Y = $o.Y; W = $r.R - $r.L; H = $r.B - $r.T }
}
function Map-Point([hashtable]$m, [int]$X, [int]$Y) {
    return @([int]($m.X + $X * $m.W / 1920.0), [int]($m.Y + $Y * $m.H / 1080.0))
}
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
# The log is rewritten from scratch every launch, and Sunrise holds it open: read it shared.
function Read-Log {
    try {
        $fs = [IO.File]::Open($log, 'Open', 'Read', 'ReadWrite')
        $sr = New-Object IO.StreamReader($fs)
        $text = $sr.ReadToEnd()
        $sr.Close()
        return $text -split "`n"
    } catch { return @() }
}
function Count-Matches([string]$Pattern) {
    return @(Read-Log | Where-Object { $_ -match $Pattern }).Count
}
function Last-Match([string]$Pattern) {
    return @(Read-Log | Where-Object { $_ -match $Pattern } | Select-Object -Last 1)
}

# Waits for one more `activity:in_world` than there were, then watches for the stall probe.
function Wait-World([int]$Before, [string]$Label) {
    $deadline = (Get-Date).AddSeconds($WorldTimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 3
        if (-not (Get-GameProcess)) { return 'died' }
        if ((Count-Matches "Entering state 'activity:in_world'") -gt $Before) { break }
        if ((Count-Matches 'stage=stall result=detected') -gt $script:stallsBefore) { return 'stall_before_world' }
    }
    if ((Count-Matches "Entering state 'activity:in_world'") -le $Before) { return 'no_world' }
    $deadline = (Get-Date).AddSeconds($AfterWorldSeconds)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 3
        if ((Count-Matches 'stage=stall result=detected') -gt $script:stallsBefore) { return 'hang' }
    }
    [void](Focus-Game)
    Save-Shot (Join-Path $shots "lh_${Tag}_${Label}.png") | Out-Null
    return 'ok'
}

Clear-MockInput
Stop-Game
Remove-Item C:\Games\Sunrise\SVR_MockXR.log, C:\Games\Sunrise\SVR_Weapon.txt -ErrorAction SilentlyContinue
[System.IO.File]::WriteAllText('C:\Games\Sunrise\SVR_Destination.txt', "$Destination`n", (New-Object System.Text.UTF8Encoding($false)))
Remove-Item C:\Games\Sunrise\SVR_Destination.off -ErrorAction SilentlyContinue
$debugFile = 'C:\Games\Sunrise\SVR_Debug.txt'
if ($Prep.Count -gt 0) {
    [System.IO.File]::WriteAllText($debugFile, (($Prep -join "`n") + "`n"), (New-Object System.Text.UTF8Encoding($false)))
} else {
    Remove-Item $debugFile -ErrorAction SilentlyContinue
}
# The eye-shaped render puts the Director's buttons somewhere else, so scripted clicks need the
# desktop shape unless the test is about the shape itself.
$displayFile = 'C:\Games\Sunrise\SVR_Display.txt'
[System.IO.File]::WriteAllText($displayFile, "shape $Shape`n", (New-Object System.Text.UTF8Encoding($false)))
$manifest = Enable-MockXr
Write-Output "tag=$Tag f9=$F9 loads=$Loads shape=$Shape prep=[$($Prep -join '; ')]"
Start-Game -Fresh | Out-Null
$p = Wait-GameWindow -TimeoutSeconds 240
if (-not $p) { throw 'no game window' }
Start-Sleep -Seconds $SettleSeconds
[void](Focus-Game)
Send-GameKey RETURN
Start-Sleep -Seconds 40
[void](Focus-Game)
$m = Get-ClientMap
$pt = Map-Point $m 1360 455
Send-GameClick -X $pt[0] -Y $pt[1]
Start-Sleep -Seconds $LoadSeconds
[void](Focus-Game)
Save-Shot (Join-Path $shots "lh_${Tag}_orbit.png") | Out-Null
$script:stallsBefore = Count-Matches 'stage=stall result=detected'

if ($F9 -ne 'none') {
    Send-GameKey F9
    Start-Sleep -Seconds 10
    Write-Output ('xr: ' + (Last-Match 'ev=vr\.xr init'))
    if ($F9 -eq 'onoff') {
        [void](Focus-Game)
        Send-GameKey F9
        Start-Sleep -Seconds 4
        Write-Output ('off: ' + (Last-Match 'toggle=off'))
    }
}

$verdicts = @()
for ($i = 1; $i -le $Loads; $i++) {
    [void](Focus-Game)
    $before = Count-Matches "Entering state 'activity:in_world'"
    $m = Get-ClientMap
    if ($i -eq 1) {
        Hover-Click $m 960 848 2.0; Start-Sleep 5      # OPEN DIRECTOR
        Save-Shot (Join-Path $shots "lh_${Tag}_nav1.png") | Out-Null
        Hover-Click $m 960 250;     Start-Sleep 6      # Tower
        Save-Shot (Join-Path $shots "lh_${Tag}_nav2.png") | Out-Null
        Hover-Click $m 908 290;     Start-Sleep 3      # landing node
        Save-Shot (Join-Path $shots "lh_${Tag}_nav3.png") | Out-Null
        Hover-Click $m 1587 884;    Start-Sleep 5      # LAUNCH
        Save-Shot (Join-Path $shots "lh_${Tag}_nav4.png") | Out-Null
    } else {
        # Back to orbit first: Director -> orbit is the menu's own path, so use the settings route.
        throw 'multi-load not implemented yet'
    }
    $v = Wait-World $before "load$i"
    $verdicts += "load$i=$v"
    Write-Output "load ${i}: $v"
    Write-Output ('  last vr line: ' + (Last-Match 'ev=vr\.'))
    Write-Output ('  world: ' + (Last-Match "Entering state"))
    if ($v -ne 'ok') { break }
}
Write-Output ("VERDICT tag=$Tag f9=$F9 " + ($verdicts -join ' '))
