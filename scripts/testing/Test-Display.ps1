# What the headset shows, measured on the mock: each eye's image as the compositor builds it from the
# submitted layer, analysed by eye_view.py (coverage and bands in degrees, pixel density and stretch,
# horizontal and vertical disparity, an anaglyph and a disparity map).
#
# Needs the game running on the mock, in a world, with F9 on (Run-Patrol.ps1). The mock must be the
# current build (scripts\build-mockxr.ps1 then a relaunch: Enable-MockXr copies it on launch).
#
#   Test-Display.ps1 -Tag base                 # one composite at the standard pose
#   Test-Display.ps1 -Tag swim -World -Seq 12  # twelve consecutive frames, eye not turning
#   Test-Display.ps1 -Tag near -NoPose         # whatever pose is current
param(
    [string]$Tag = 'view',
    [switch]$World,
    [int]$Seq = 0,
    [switch]$Full,
    [switch]$NoPose,
    # The standard pose: head level, the right hand raised in front so the gun is in view.
    [double]$HeadYaw = 0, [double]$HeadPitch = 0,
    [double]$HandX = 0.0, [double]$HandY = 0.12, [double]$HandZ = 0.0,
    [double]$HandYaw = 0, [double]$HandPitch = 0,
    [double]$SettleSeconds = 2.0,
    [int]$TimeoutSeconds = 30
)
$ErrorActionPreference = 'Stop'
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'lib\GameIO.ps1')
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$out = Join-Path $root 'build\shots\display'
if (-not (Test-Path $out)) { New-Item -ItemType Directory -Force -Path $out | Out-Null }
$game = 'C:\Games\Sunrise'

if (-not (Get-GameProcess)) { throw 'the game is not running' }
[void](Assert-GameFocus)
if (-not $NoPose) {
    [void](Set-MockInput -HeadYaw $HeadYaw -HeadPitch $HeadPitch -HandX $HandX -HandY $HandY -HandZ $HandZ `
        -HandYaw $HandYaw -HandPitch $HandPitch)
}
Start-Sleep -Seconds $SettleSeconds

$last = if ($Seq -gt 1) { '{0}_f{1:D2}' -f $Tag, ($Seq - 1) } else { $Tag }
Get-ChildItem $game -Filter "SVR_seen_$Tag*" -ErrorAction SilentlyContinue | Remove-Item -Force
$words = @($Tag)
if ($World) { $words += 'world' }
if ($Full) { $words += 'full' }
if ($Seq -gt 1) { $words += @('seq', $Seq) }
[System.IO.File]::WriteAllText((Join-Path $game 'SVR_MockCompose.txt'), (($words -join ' ') + "`n"),
    (New-Object System.Text.UTF8Encoding($false)))
$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
while (-not (Test-Path (Join-Path $game "SVR_seen_$last.txt"))) {
    if ((Get-Date) -gt $deadline) { throw "no composite after $TimeoutSeconds s (is the mock current and F9 on?)" }
    Start-Sleep -Milliseconds 200
}
Start-Sleep -Milliseconds 300
Get-ChildItem $game -Filter "SVR_seen_$Tag*" | Copy-Item -Destination $out -Force
$py = Join-Path $PSScriptRoot 'eye_view.py'
if ($Seq -gt 1) {
    & python $py seq $out $Tag $Seq
    & python $py $out ('{0}_f00' -f $Tag) --out $out --json
} else {
    & python $py $out $Tag --out $out --json
}
