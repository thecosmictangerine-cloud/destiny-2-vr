# Which signal says "a menu is open in the world"? Lands in the forced destination with VR on (the
# landing is driven like Test-Lifecycle: the laser), then opens and closes each menu with the
# keyboard while `stateprobe` logs the pointer's state every 30 presents (vr_state.cpp), with a
# screenshot of each. Read the result with the `ev=vr.state probe` lines around each `MARK`.
param(
    [string]$Destination = 'eden_freeroam 20 160 7',
    [string]$Tag = 'menus'
)
$ErrorActionPreference = 'Stop'
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'lib\GameIO.ps1')
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$shots = Join-Path $repo 'build\shots'
$game = 'C:\Games\Sunrise'
$log = Join-Path $game 'bin\x64\Sunrise\logs\sunrise.log'

function Mark([string]$Text) {
    # A line of our own in the game's log is not possible from here; the time is recorded instead.
    $line = '{0} MARK {1}' -f (Get-Date -Format 'HH:mm:ss.fff'), $Text
    Add-Content -Path (Join-Path $shots "${Tag}_marks.txt") -Value $line
    Write-Output $line
}
Remove-Item (Join-Path $shots "${Tag}_marks.txt") -ErrorAction SilentlyContinue

Clear-MockInput
Stop-Game
[System.IO.File]::WriteAllText((Join-Path $game 'SVR_Destination.txt'), "$Destination`n", (New-Object System.Text.UTF8Encoding($false)))
[System.IO.File]::WriteAllText((Join-Path $game 'SVR_Display.txt'), "shape desktop`n", (New-Object System.Text.UTF8Encoding($false)))
[System.IO.File]::WriteAllText((Join-Path $game 'SVR_Debug.txt'), "stateprobe`n", (New-Object System.Text.UTF8Encoding($false)))
$manifest = Enable-MockXr
Start-Game -Fresh | Out-Null
$p = Wait-GameWindow -TimeoutSeconds 240
if (-not $p) { throw 'no game window' }
Start-Sleep -Seconds 70
[void](Focus-Game)
Send-GameKey RETURN
Start-Sleep -Seconds 40
[void](Focus-Game)
Send-GameClick -X 1360 -Y 455
Start-Sleep -Seconds 85
[void](Focus-Game)
# Director -> Tower -> landing -> LAUNCH (VR off: the desktop coordinates hold).
foreach ($step in @(@(960, 848, 5), @(960, 250, 6), @(908, 290, 3), @(1587, 884, 5))) {
    [void][SunriseVR.Native]::SetCursorPos($step[0] - 9, $step[1] - 5)
    Start-Sleep -Milliseconds 300
    [SunriseVR.Native]::mouse_event(0x0001, 5, 3, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 300
    [SunriseVR.Native]::mouse_event(0x0001, 4, 2, 0, [IntPtr]::Zero)
    Start-Sleep -Seconds 1.2
    [SunriseVR.Native]::mouse_event(0x0002, 0, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 80
    [SunriseVR.Native]::mouse_event(0x0004, 0, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Seconds $step[2]
}
Start-Sleep -Seconds 100
[void](Focus-Game)
Mark 'world'
Start-Sleep -Seconds 4
foreach ($key in @('F1', 'ESCAPE', 'TAB', 'M')) {
    [void](Focus-Game)
    Mark "open $key"
    Send-GameKey $key
    Start-Sleep -Seconds 4
    Save-Shot (Join-Path $shots "${Tag}_$key.png") | Out-Null
    Mark "close $key"
    Send-GameKey ESCAPE
    Start-Sleep -Seconds 4
    if ($key -eq 'ESCAPE') {
        # Esc opened a menu; a second Esc may be needed to be back in the world.
        Send-GameKey ESCAPE
        Start-Sleep -Seconds 3
    }
    Mark "world after $key"
    Save-Shot (Join-Path $shots "${Tag}_after_$key.png") | Out-Null
    Start-Sleep -Seconds 3
}
Copy-Item $log (Join-Path $shots "${Tag}_sunrise.log") -Force
Remove-Item (Join-Path $game 'SVR_Debug.txt') -ErrorAction SilentlyContinue
