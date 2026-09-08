# Checks the three claims the camera/body arrangement rests on, with numbers rather than opinion.
#
#   1. The servo converges. Turn the head, and the character's facing catches up on its own, so
#      `ev=vr.body error` falls to about zero without anything being injected by hand.
#   2. The horizon does NOT drift while it converges. This is the whole reason the anchor was moved
#      off the character: if the view still hung from the character's yaw, every correction would
#      rotate the world under the player. Two captures taken while the servo is working and after
#      it has finished must show the same world.
#   3. Walking goes where the player is LOOKING. Head turned, stick forward, and the position delta
#      from the log has to point along the head's direction rather than the character's old one.
#
# Wants the game in a first-person destination with F9 on: run Run-Patrol.ps1 first.
param(
    [string]$GameDir = 'C:\Games\Sunrise',
    [string]$Tag = 's1',
    [double]$HeadYaw = 60,
    [int]$WalkSeconds = 8
)
$ErrorActionPreference = 'Stop'
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'lib\GameIO.ps1')
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$shots = Join-Path $repo 'build\shots'
$log = Join-Path $GameDir 'bin\x64\Sunrise\logs\sunrise.log'

function Read-LogLines {
    $stream = [IO.File]::Open($log, 'Open', 'Read', 'ReadWrite')
    try {
        $reader = New-Object IO.StreamReader($stream)
        try { return $reader.ReadToEnd() -split "`r?`n" } finally { $reader.Dispose() }
    } finally { $stream.Dispose() }
}
function Last-Body {
    $hit = (Read-LogLines) | Select-String 'ev=vr\.body ' | Select-Object -Last 1
    if (-not $hit) { return $null }
    $line = $hit.Line
    $out = @{}
    foreach ($field in 'anchor', 'head_yaw', 'body_yaw', 'error', 'counts_per_rad') {
        if ($line -match "$field=(-?[\d.]+)") { $out[$field] = [double]$Matches[1] }
    }
    $out['raw'] = ($line -replace '.*ev=vr\.body ', '')
    return $out
}
# Positions the module logs, newest last.
function Positions {
    (Read-LogLines) | Select-String 'ev=vr\.probe pose in_pos=' | ForEach-Object {
        if ($_.Line -match 'in_pos=(-?[\d.]+),(-?[\d.]+),(-?[\d.]+)') {
            , @([double]$Matches[1], [double]$Matches[2], [double]$Matches[3])
        }
    }
}
function S([string]$n) { Start-Sleep -Seconds 1; [void](Focus-Game); Save-Shot (Join-Path $shots ("{0}_{1}.png" -f $Tag, $n)) | Out-Null }

$zero = @{ HeadYaw = 0; HeadPitch = 0; HeadX = 0; HeadY = 0; HeadZ = 0
    HandYaw = 0; HandPitch = 0; HandX = 0; HandY = 0; HandZ = 0
    MoveX = 0; MoveY = 0; TurnX = 0; TurnY = 0
    LHandYaw = 0; LHandPitch = 0; LHandX = 0; LHandY = 0; LHandZ = 0 }
function With { param([hashtable]$c) $m = $zero.Clone(); foreach ($k in $c.Keys) { $m[$k] = $c[$k] }; return $m }

if (-not (Get-GameProcess)) { throw 'game is not running; run Run-Patrol.ps1 first' }
[void](Focus-Game)
if (-not ((Read-LogLines) | Select-String 'ev=vr\.xr init result=ok')) { throw 'no OpenXR session; press F9 in world' }

Write-Output '== 1. does the servo converge? =='
Set-MockInput @zero | Out-Null
Start-Sleep -Seconds 6
$rest = Last-Body
Write-Output ("  at rest        " + $rest.raw)

$m = With @{ HeadYaw = $HeadYaw }; Set-MockInput @m | Out-Null
Start-Sleep -Seconds 2
$during = Last-Body
Write-Output ("  1 s after turn " + $during.raw)
S 'a_during'
Start-Sleep -Seconds 8
$settled = Last-Body
Write-Output ("  8 s after turn " + $settled.raw)
S 'b_settled'

$expected = [Math]::Abs($HeadYaw) * [Math]::PI / 180.0
Write-Output ''
Write-Output ("  head turned {0:N1} deg = {1:N3} rad" -f $HeadYaw, $expected)
Write-Output ("  head_yaw reported        {0:N3} rad" -f $settled.head_yaw)
Write-Output ("  residual error at rest   {0:N4} rad ({1:N2} deg)" -f $settled.error, ($settled.error * 180 / [Math]::PI))
Write-Output ("  learned counts per rad   {0:N0}" -f $settled.counts_per_rad)
if ([Math]::Abs($settled.error) -lt 0.05) {
    Write-Output '  PASS  the character caught up with the head'
} else {
    Write-Output '  FAIL  the character did not catch up'
}

Write-Output ''
Write-Output '== 2. did the horizon stay put while it caught up? =='
python (Join-Path $PSScriptRoot 'diff_panel.py') $shots ("panel_{0}.png" -f $Tag) --full `
    ("horizon during vs settled={0}_a_during.png,{0}_b_settled.png" -f $Tag)
Write-Output '  (a mean abs diff near the scene noise floor means the world did not move)'

Write-Output ''
Write-Output ("== 3. does walking go where the head is looking? ==")
$before = @(Positions)
$m = With @{ HeadYaw = $HeadYaw; MoveY = 1.0 }; Set-MockInput @m | Out-Null
Start-Sleep -Seconds $WalkSeconds
$m = With @{ HeadYaw = $HeadYaw }; Set-MockInput @m | Out-Null
Start-Sleep -Seconds 4
$after = @(Positions)
if ($after.Count -le $before.Count) {
    Write-Output '  no fresh position samples; cannot judge'
} else {
    $from = $before[-1]
    $to = $after[-1]
    $dx = $to[0] - $from[0]; $dy = $to[1] - $from[1]
    $travelled = [Math]::Sqrt($dx * $dx + $dy * $dy)
    Write-Output ("  from {0:N2},{1:N2} to {2:N2},{3:N2}  travelled {4:N2} units" -f $from[0], $from[1], $to[0], $to[1], $travelled)
    if ($travelled -lt 1.0) {
        Write-Output '  did not move; the movement keys are not reaching the game'
    } else {
        $moveYaw = [Math]::Atan2($dy, $dx)
        $bodyYaw = $settled.body_yaw
        $lookYaw = $settled.anchor + $settled.head_yaw
        function Wrap([double]$a) { [Math]::Atan2([Math]::Sin($a), [Math]::Cos($a)) }
        Write-Output ("  travel direction {0:N3} rad | look {1:N3} | character {2:N3}" -f $moveYaw, (Wrap $lookYaw), $bodyYaw)
        $offLook = [Math]::Abs((Wrap ($moveYaw - $lookYaw)) * 180 / [Math]::PI)
        Write-Output ("  off the look direction by {0:N1} deg" -f $offLook)
        if ($offLook -lt 25) { Write-Output '  PASS  walking follows the gaze' } else { Write-Output '  FAIL  walking does not follow the gaze' }
    }
}
Set-MockInput @zero | Out-Null
