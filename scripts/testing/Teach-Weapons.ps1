# Teaches the ADS module every weapon in the loadout, so a headset session starts with all of them
# known (vr_ads.cpp learns a weapon the first time it is held in ADS; until then the physical detector
# works from a guessed sight and the first ADS is not compensated).
#
# For each slot: draw it, hold still at the hip until its hip pose is matched or a new profile is
# started, hold ADS (`force on`) long enough to learn the ADS pose, then a few short cycles for the
# ADS curve. Profiles land in C:\Games\Sunrise\SVR_AdsProfiles.txt, which the headset session reads.
param(
    [string[]]$Slots = @('1', '2', '3'),
    [int]$Cycles = 4
)
$ErrorActionPreference = 'Stop'
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'lib\GameIO.ps1')
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'lib\VrCommands.ps1')
$front = @{ HandX = 0.05; HandY = 0.12; HandZ = -0.20; HandYaw = -8; HandPitch = -6 }
[void](Focus-Game)
[void](Set-MockInput @front)
[void](Send-Ads @('force off', 'comp on', 'learn on'))
foreach ($slot in $Slots) {
    Send-GameKey $slot
    Start-Sleep -Seconds 4
    [void](Send-Ads @('force on'))
    Start-Sleep -Seconds 3
    [void](Send-Ads @('force off'))
    Start-Sleep -Seconds 2
    for ($i = 0; $i -lt $Cycles; $i++) {
        [void](Send-Ads @('force on'))
        Start-Sleep -Seconds 1.5
        [void](Send-Ads @('force off'))
        Start-Sleep -Seconds 1.5
    }
    [void](Send-Ads @('dump'))
    Start-Sleep -Milliseconds 600
    Write-Host "--- slot $slot"
    Get-VrLog 'ev=vr\.ads (weapon|state=)' 3 | ForEach-Object { Write-Host $_.Substring($_.IndexOf('ev=')) }
}
Send-GameKey $Slots[0]
Start-Sleep -Seconds 3
[void](Send-Ads @('dump'))
Start-Sleep -Milliseconds 600
Get-VrLog 'ev=vr\.ads (profile|curve) ' 12 | ForEach-Object { Write-Host $_.Substring($_.IndexOf('ev=')) }
