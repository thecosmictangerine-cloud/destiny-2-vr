# A super in first person (docs/SWORD-SPARROW.md), on the mock, in a session already in the world.
# Sunrise has no enemies, so the super charges by itself (minutes): the script presses it (the right stick
# click = F) every 20 s, nudging the head so the game does not send an idle player to orbit, and once it
# comes up -- the engine's third-person camera, mode on_foot -- composes the eyes, takes a GPU capture and
# logs what the body module made of it (the super's weapon, found beside the hand like a sword).
# Results in build\shots\super_<tag>\.

param(
    [string]$Tag = 'super',
    [int]$WaitMinutes = 12,
    [string]$Out = ''
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\lib\GameIO.ps1')
. (Join-Path $PSScriptRoot '..\lib\VrSession.ps1')
. (Join-Path $PSScriptRoot '..\lib\VrCommands.ps1')
if (-not $Out) { $Out = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) "build\shots\super_$Tag" }
New-Item -ItemType Directory -Force $Out | Out-Null

$rest = @{ HandX = 0.05; HandY = 0.15; HandZ = -0.1; LHandX = -0.05; LHandY = 0.12; LHandZ = -0.1 }
Assert-GameFocus -Quiet | Out-Null
Set-MockInput @rest | Out-Null
Send-GameKey '1'; Start-Sleep 3

$deadline = (Get-Date).AddMinutes($WaitMinutes)
$active = $null
$nudge = 0
while ((Get-Date) -lt $deadline -and -not $active) {
    $t0 = Log-Now
    Set-MockInput @rest -ThumbR 1 | Out-Null; Start-Sleep -Milliseconds 250; Set-MockInput @rest | Out-Null
    $active = Wait-LogSince $t0 'ev=vr\.mode mode=on_foot' 6
    if (-not $active) {
        $nudge = ($nudge + 1) % 4
        Set-MockInput @rest -HeadYaw (@(0, 8, 0, -8)[$nudge]) | Out-Null
        Start-Sleep 14
        "{0:HH:mm:ss} waiting for the super" -f (Get-Date)
    }
}
if (-not $active) { "SUPER never came up in $WaitMinutes min"; exit 1 }
"SUPER up: $active"
Start-Sleep -Milliseconds 800
Compose-Eye "${Tag}_a" $Out | Out-Null
Invoke-GpuCapture | Out-Null
Copy-Item (Join-Path $script:svrGame 'SVR_GpuCapture.txt') (Join-Path $Out "cap_$Tag.txt") -Force
Copy-Item (Join-Path $script:svrGame 'SVR_GpuCapture.bin') (Join-Path $Out "cap_$Tag.bin") -Force
# The weapon in hand: raise it, swing it, attack once.
$up = $rest.Clone(); $up['HandPitch'] = 50; $up['HandY'] = 0.3
Set-MockInput @up | Out-Null; Start-Sleep -Milliseconds 800
Compose-Eye "${Tag}_b" $Out | Out-Null
$side = $rest.Clone(); $side['HandYaw'] = -50; $side['HandX'] = 0.25
Set-MockInput @side | Out-Null; Start-Sleep -Milliseconds 800
Compose-Eye "${Tag}_c" $Out | Out-Null
Set-MockInput @rest -TrigR 1 | Out-Null; Start-Sleep -Milliseconds 300; Set-MockInput @rest | Out-Null
Start-Sleep -Milliseconds 400
Compose-Eye "${Tag}_d" $Out | Out-Null
Set-MockInput @rest -HeadPitch -60 | Out-Null; Start-Sleep -Milliseconds 800
Compose-Eye "${Tag}_down" $Out | Out-Null
Set-MockInput @rest | Out-Null
$t1 = Get-LineTime $active
Log-Since ($t1 - 1) 'ev=vr\.(mode|body) ' | Where-Object { $_ -notmatch 'trace' } | Select-Object -First 30 | ForEach-Object { $_.Substring(0, [Math]::Min(240, $_.Length)) }
$end = Wait-LogSince $t1 'ev=vr\.mode mode=first was=on_foot' 40
"SUPER ended: $end"
