# Full cycle: launch on the mock, reach orbit, force mission_ember/1AU through Sunrise's Activity
# override (session-only, so it is redone every launch), launch it via the Director, then toggle
# VR and capture. Coordinates are physical pixels on the 1920x1080 desktop.
param([int]$SettleSeconds = 70, [int]$LoadSeconds = 85, [int]$MissionSeconds = 100, [switch]$NoToggle)
$ErrorActionPreference = 'Stop'
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'lib\GameIO.ps1')
$shots = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'build\shots')
if (-not (Test-Path $shots)) { New-Item -ItemType Directory -Force -Path $shots | Out-Null }

# The Director's nodes need real pointer motion over them before a click counts as a select.
function Hover-Click([int]$X, [int]$Y, [double]$Settle = 1.0) {
    [void][SunriseVR.Native]::SetCursorPos($X - 9, $Y - 5)
    Start-Sleep -Milliseconds 300
    [SunriseVR.Native]::mouse_event(0x0001, 5, 3, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 300
    [SunriseVR.Native]::mouse_event(0x0001, 4, 2, 0, [IntPtr]::Zero)
    Start-Sleep -Seconds $Settle
    [SunriseVR.Native]::mouse_event(0x0002, 0, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 80
    [SunriseVR.Native]::mouse_event(0x0004, 0, 0, 0, [IntPtr]::Zero)
}

Clear-MockInput
Remove-Item C:\Games\Sunrise\SVR_MockXR.log -ErrorAction SilentlyContinue
$manifest = Enable-MockXr
Write-Output "mock manifest: $manifest"
Start-Game -Fresh | Out-Null
$p = Wait-GameWindow -TimeoutSeconds 240
if (-not $p) { throw 'no game window' }
Start-Sleep -Seconds $SettleSeconds
[void](Focus-Game)
Send-GameKey RETURN
Start-Sleep -Seconds 40
[void](Focus-Game)
Send-GameClick -X 1360 -Y 455          # Hunter
Start-Sleep -Seconds $LoadSeconds
[void](Focus-Game)
Save-Shot (Join-Path $shots 'e_orbit.png') | Out-Null
Write-Output ('orbit: ' + (Get-GameStats))

# Sunrise menu: Activity override -> mission_ember / 281 1AU / bubble 6 / slice 49, enabled.
Send-GameKey INSERT;            Start-Sleep 2
Send-GameClick -X 455 -Y 397;   Start-Sleep 1.5       # Activity module
Send-GameClick -X 1090 -Y 429;  Start-Sleep 1.5       # package dropdown
Send-GameText -Text 'ember';    Start-Sleep 1.5
Send-GameClick -X 673 -Y 613;   Start-Sleep 1.5       # mission_ember
Send-GameClick -X 1090 -Y 509;  Start-Sleep 1.5       # activity dropdown
Send-GameClick -X 668 -Y 631;   Start-Sleep 1.5       # 281 1AU
Send-GameClick -X 1090 -Y 589;  Start-Sleep 1.5       # bubble dropdown
Send-GameClick -X 712 -Y 867;   Start-Sleep 1.5       # bubble 6
Send-GameClick -X 1090 -Y 669;  Start-Sleep 1.5       # slice dropdown
Send-GameClick -X 656 -Y 791;   Start-Sleep 1.5       # slice 49
Send-GameClick -X 1541 -Y 349;  Start-Sleep 1.5       # Enabled
Save-Shot (Join-Path $shots 'e_override.png') | Out-Null
Send-GameKey INSERT;            Start-Sleep 1.5

# Director: Tower -> landing node -> LAUNCH. The override redirects the load.
Send-GameClick -X 960 -Y 848;   Start-Sleep 3          # OPEN DIRECTOR
Hover-Click 960 250;            Start-Sleep 6          # Tower
Hover-Click 908 290;            Start-Sleep 3          # landing node
Save-Shot (Join-Path $shots 'e_launch_panel.png') | Out-Null
Hover-Click 1587 884;           Start-Sleep 5          # LAUNCH
Save-Shot (Join-Path $shots 'e_launching.png') | Out-Null
Start-Sleep -Seconds $MissionSeconds
[void](Focus-Game)
Save-Shot (Join-Path $shots 'e_arrived.png') | Out-Null
Write-Output ('arrived: ' + (Get-GameStats))

if (-not $NoToggle) {
    Send-GameKey F9
    Start-Sleep -Seconds 6
    Save-Shot (Join-Path $shots 'e_xr_a.png') | Out-Null
    Start-Sleep -Seconds 5
    Save-Shot (Join-Path $shots 'e_xr_b.png') | Out-Null
    Write-Output ('after F9: ' + (Get-GameStats))
}
Write-Output '---- sunrise.log (vr)'
Select-String -Path C:\Games\Sunrise\bin\x64\Sunrise\logs\sunrise.log -Pattern 'vr\.' | ForEach-Object { $_.Line.Substring(0, [Math]::Min(240, $_.Line.Length)) } | Select-Object -Last 30
