# Full cycle: launch on the mock, reach orbit, toggle VR, capture, dump logs.
param([switch]$NoToggle, [int]$SettleSeconds = 70, [int]$LoadSeconds = 85)
$ErrorActionPreference = 'Stop'
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'lib\GameIO.ps1')
$shots = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'build\shots')
if (-not (Test-Path $shots)) { New-Item -ItemType Directory -Force -Path $shots | Out-Null }
Clear-MockInput
# A running (or hung) game holds the mock DLL, and Enable-MockXr overwrites it.
Stop-Game
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
Save-Shot (Join-Path $shots 'r_charsel.png') | Out-Null
Send-GameClick -X 1360 -Y 455
Start-Sleep -Seconds $LoadSeconds
[void](Focus-Game)
Save-Shot (Join-Path $shots 'r_orbit.png') | Out-Null
Write-Output ('orbit: ' + (Get-GameStats))
if (-not $NoToggle) {
    Send-GameKey F9
    Start-Sleep -Seconds 6
    Save-Shot (Join-Path $shots 'r_xr_a.png') | Out-Null
    Start-Sleep -Seconds 5
    Save-Shot (Join-Path $shots 'r_xr_b.png') | Out-Null
    Write-Output ('after F9: ' + (Get-GameStats))
}
Write-Output '---- sunrise.log (vr)'
Select-String -Path C:\Games\Sunrise\bin\x64\Sunrise\logs\sunrise.log -Pattern 'vr\.' | ForEach-Object { $_.Line.Substring(0, [Math]::Min(240, $_.Line.Length)) } | Select-Object -First 40
Write-Output '---- SVR_MockXR.log'
Get-Content C:\Games\Sunrise\SVR_MockXR.log -ErrorAction SilentlyContinue | Select-Object -First 30
