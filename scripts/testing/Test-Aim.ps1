# Shots leave the barrel: the matrix, measured, in a running session (a first-person area, F9 on).
#
# Each step poses the head and the right controller on the mock, fires one round with a real click,
# and finds where it hit (impact_find.py --decal: the mark the round leaves, in a capture 2.5 s
# later, against one from just before and a null pair as far apart). The mark, not the flash: this
# hand cannon's rounds explode, and the flash is centred ~20 cm above the point hit, which read as a
# steady 1-1.5 deg "miss" upwards before the reference was changed. The flash's miss is still
# reported beside it (`flash`), for comparison. The aim line for that pose (`ev=vr.aim line`, the
# barrel as the engine drew the gun, projected with the camera the world was rendered with) is the
# reference: the miss is the impact's angular distance from it. Because the line is projected by the
# frame's own camera, the comparison needs no depth at all -- an impact on the shot's line lies on
# the projected line at whatever distance it happens.
#
# While measuring, the right controller is held low and out to the side, OUT OF FRAME, aiming back
# into the scene, and the drawn line is off: the gun's recoil would otherwise be the biggest change
# between the two captures, and the impact of a shot aimed at nearby ground lands right beside the
# gun. Where the gun is changes nothing about the claim -- the round must leave its muzzle along its
# barrel wherever that is -- and `Show-Aim.ps1` is the in-frame picture for a human to look at.
#
# The steps that decide it:
#   hand_*      the controller moves, the head does not: the impact follows the controller.
#   head_*      the head moves, the controller does not: the impact stays on the barrel's line.
#   roll        a wrist roll changes nothing about the bore.
#   turn        an artificial turn moves both together.
#   far         level, at whatever the line meets far away.
#   near        the barrel down at the ground a metre or two away: where the shot starts matters
#               there, so an origin left at the eye would show as a miss along the line's normal.
#   game_*      `shot observe`: the game's own shot, for contrast -- it must FAIL the same check, or
#               the test cannot tell the difference and proves nothing.
#
# Every step also checks that its shot is a NEW one (serial), that it was or was not rewritten as the
# step expects, and that the line it is measured against has the direction the round was fired along.
param(
    [string]$Out = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'build\shots\aimtest'),
    [double]$Settle = 1.6,
    [int]$FlashMs = 90,
    [int]$DecalMs = 2500,
    [double]$PassDeg = 1.0,
    [double]$SelfDeg = 2.5,
    [string]$Prefix = '',
    [int]$Magazine = 10,
    [string[]]$Only = @(),
    # Adds the two-handed steps; needs a long gun in hand (the rifle, slot 2).
    [switch]$TwoHand
)
$ErrorActionPreference = 'Stop'
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'lib\GameIO.ps1')
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'lib\VrCommands.ps1')
if (-not (Test-Path $Out)) { New-Item -ItemType Directory -Force -Path $Out | Out-Null }
$py = Join-Path $PSScriptRoot 'impact_find.py'
$inv = [System.Globalization.CultureInfo]::InvariantCulture

# The controller out of frame, low on the right, aiming forward and a little left into the view.
$base = @{ HandX = 0.70; HandY = -0.45; HandZ = 0.15; HandYaw = 15; HandPitch = -5 }

function Fire {
    [SunriseVR.Native]::mouse_event(0x0002, 0, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 60
    [SunriseVR.Native]::mouse_event(0x0004, 0, 0, 0, [IntPtr]::Zero)
}

function Get-AimLine {
    $before = @(Read-VrLog 4000 | Where-Object { $_ -match 'ev=vr\.aim line' }).Count
    [void](Send-Aim @('dump'))
    for ($i = 0; $i -lt 20; $i++) {
        Start-Sleep -Milliseconds 150
        $lines = @(Read-VrLog 4000 | Where-Object { $_ -match 'ev=vr\.aim line' })
        if ($lines.Count -gt $before) { return $lines[-1] }
    }
    return $null
}

function Get-Vec([string]$text, [string]$name) {
    if ($text -notmatch "$name=([-\d.]+),([-\d.]+),([-\d.]+)") { return $null }
    return @([double]::Parse($Matches[1], $inv), [double]::Parse($Matches[2], $inv), [double]::Parse($Matches[3], $inv))
}
function Get-Serial {
    $s = Get-VrLog 'ev=vr\.aim shot ' 1
    if ("$s" -match 'serial=(\d+)') { return [int]$Matches[1] }
    return 0
}

# Reload every few steps so a step never clicks on an empty gun.
$script:shotsSinceReload = 0
function Reload-IfLow {
    if ($script:shotsSinceReload -ge [Math]::Max(1, $Magazine - 2)) {
        Send-GameKey R
        Start-Sleep -Seconds 3.2
        $script:shotsSinceReload = 0
    }
}

function Get-Hold {
    [void](Send-Arms @('twohand dump'))
    Start-Sleep -Milliseconds 350
    $line = Get-VrLog 'ev=vr\.arms hold state=' 1
    $h = @{}
    foreach ($m in [regex]::Matches("$line", '(\w+)=([^\s]+)')) { $h[$m.Groups[1].Value] = $m.Groups[2].Value }
    return $h
}
$script:noJitter = $false
$script:lastShot = ''

# The character's own pitch, by real mouse motion in steps of 60 counts (positive = down). Tracked so
# the finally block can always level it again.
$script:pitched = 0
function Pitch-Character([int]$steps) {
    $delta = if ($steps -gt 0) { [uint32]60 } else { [uint32]4294967236 }  # -60, two's complement
    for ($i = 0; $i -lt [Math]::Abs($steps); $i++) {
        [SunriseVR.Native]::mouse_event(0x0001, 0, $delta, 0, [IntPtr]::Zero)
        Start-Sleep -Milliseconds 40
    }
    $script:pitched += $steps
}

$results = New-Object System.Collections.ArrayList
# Step kinds:
#   barrel    `shot aim`: the impact must lie on the ray the round was fired along (the barrel).
#   contrast  `shot observe`: the game's own shot must MISS the barrel line by far.
#   self      `shot observe` with the character itself aimed at the ground: the game's own shot
#             measured against its OWN ray (eye along the character's aim). This calibrates the
#             instrument -- whatever offset shows up here is the measurement's (the height of an
#             explosion's flash above the point hit, the weapon's spread), not the redirect's.
function Step([string]$name, [hashtable]$pose, [string]$kind = 'barrel') {
    $expectOnLine = $kind -ne 'contrast'
    $redirected = $kind -eq 'barrel'
    if ($Only.Count -gt 0 -and $Only -notcontains $name) { return }
    Reload-IfLow
    $p = @{} + $base
    foreach ($k in $pose.Keys) { $p[$k] = $pose[$k] }
    # A small offset per run, the same for every step: marks persist, and a round landing on the
    # last run's mark leaves no new one to find.
    if (-not $script:noJitter) {
        $p['HandYaw'] = [double]$(if ($p.ContainsKey('HandYaw')) { $p['HandYaw'] } else { 0 }) + $script:jitter[0]
        $p['HandPitch'] = [double]$(if ($p.ContainsKey('HandPitch')) { $p['HandPitch'] } else { 0 }) + $script:jitter[1]
    }
    [void](Set-MockInput @p)
    Start-Sleep -Seconds $Settle
    $line = Get-AimLine
    $tag = "$Prefix$name"
    $n0 = Join-Path $Out "${tag}_n.png"
    $a = Join-Path $Out "${tag}_a.png"
    $b = Join-Path $Out "${tag}_b.png"
    $c = Join-Path $Out "${tag}_c.png"
    $sheet = Join-Path $Out "${tag}_impact.png"
    $flashSheet = Join-Path $Out "${tag}_flash.png"
    $serial0 = Get-Serial
    # Null pair first, as far apart as the before and the late capture: the scene's own motion
    # over that interval (grass, a blinking beacon) is masked out.
    Save-Shot $n0 | Out-Null
    Start-Sleep -Milliseconds $DecalMs
    Save-Shot $a | Out-Null
    $t0 = Get-Date
    Fire
    $script:shotsSinceReload++
    Start-Sleep -Milliseconds $FlashMs
    Save-Shot $b | Out-Null
    $wait = $DecalMs - ((Get-Date) - $t0).TotalMilliseconds
    if ($wait -gt 0) { Start-Sleep -Milliseconds $wait }
    # The mark the round left, once the flash and dust are gone: the reference (impact_find --decal).
    Save-Shot $c | Out-Null
    $shot = Get-VrLog 'ev=vr\.aim shot ' 1
    $ray = Get-VrLog 'ev=vr\.aim ray serial' 1
    $serial = if ("$shot" -match 'serial=(\d+)') { [int]$Matches[1] } else { 0 }
    $fresh = $serial -gt $serial0
    $rew = if ("$shot" -match 'rewritten=(\d)') { $Matches[1] } else { '?' }
    $turned = if ("$shot" -match 'turned_deg=([-\d.]+)') { $Matches[1] } else { '?' }
    # The line measured against must be the direction the round left along (camera-relative and
    # world directions share the game's axes). A line taken from a different pose would make every
    # number below meaningless.
    $lineDir = Get-Vec "$line" 'dir'
    $fired = if ($redirected) { Get-Vec "$shot" 'new_fwd' } else { Get-Vec "$shot" 'orig_fwd' }
    $lineAgree = if ($lineDir -and $fired) {
        $d = $lineDir[0] * $fired[0] + $lineDir[1] * $fired[1] + $lineDir[2] * $fired[2]
        [Math]::Acos([Math]::Min(1.0, [Math]::Max(-1.0, $d))) * 180 / [Math]::PI
    } else { [double]::NaN }
    # On-line steps are measured against the ray the round was really fired along, projected with
    # the frame's camera: the camera sits at the muzzle minus the line's camera-relative origin. The
    # contrast steps keep the barrel line itself -- what the game's shot should have followed.
    $extra = @()
    $newPos = Get-Vec "$shot" 'new_pos'
    $eye = Get-Vec "$shot" 'eye'
    $lineOrigin = Get-Vec "$line" 'origin'
    $start = $null
    $cam = $null
    if ($redirected -and $newPos -and $lineOrigin) {
        $start = $newPos
        $cam = @(0..2 | ForEach-Object { $newPos[$_] - $lineOrigin[$_] })
    } elseif ($kind -eq 'self' -and $eye) {
        # The flat game's round leaves the eye's line along the character's aim (the trace's own
        # flags put it there), so that is the ray; the camera is at the eye on the mock.
        $start = $eye
        $cam = $eye
    }
    if ($start -and $fired -and $cam) {
        $extra = @('--ray', ((@($start + $fired) | ForEach-Object { $_.ToString('R', $inv) }) -join ','),
                   '--cam', (($cam | ForEach-Object { $_.ToString('R', $inv) }) -join ','))
    }
    # PowerShell 5.1 turns any stderr line of a native command (a numpy warning) into a terminating
    # error under 'Stop', so the analysis runs under 'Continue' with stderr dropped.
    $eap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $r = & python $py $a $c $sheet --line "$line" --null $n0 @extra --decal --json 2>$null | ConvertFrom-Json
    $f = & python $py $a $b $flashSheet --line "$line" @extra --json 2>$null | ConvertFrom-Json
    $ErrorActionPreference = $eap
    $flashMiss = if ($f.found -and $f.miss_deg -ne $null) { [double]$f.miss_deg } else { [double]::NaN }
    $miss = if ($r.found -and $r.miss_deg -ne $null) { [double]$r.miss_deg } else { [double]::NaN }
    # A contrast round goes down the character's own aim, the same for every contrast and self step,
    # so it tends to land in an earlier step's mark. Its flash is enough there: the flash sits ~1 deg
    # off the point hit, against a bar of 3.
    if ($kind -eq 'contrast' -and -not $r.found -and $f.found -and $f.miss_deg -ne $null) {
        $r = $f
        $miss = [double]$f.miss_deg
    }
    $rewOk = if ($redirected) { $rew -eq '1' } else { $rew -eq '0' }
    # `self` is the game's own hip-fire round against its own ray: its bar is the weapon's spread.
    $bar = if ($kind -eq 'self') { $SelfDeg } else { $PassDeg }
    $ok = $fresh -and $rewOk -and $r.found -and $(if ($expectOnLine) { $miss -le $bar } else { $miss -gt 3 * $PassDeg })
    # No mark found is not a miss: Io's lighting drifts, and on sand lit by day a mark can be too
    # faint to see. It is reported as such, never counted as a pass.
    $verdict = if ($ok) { 'OK' } elseif ($fresh -and $rewOk -and -not $r.found) { 'INCONCLUSIVE' } else { 'FAIL' }
    $impact = if ($r.found) { ('{0:F0},{1:F0}' -f $r.x, $r.y) } else { '-' }
    $offset = if ($r.offset_px) { ('{0:F0},{1:F0}' -f $r.offset_px[0], $r.offset_px[1]) } else { '-' }
    [void]$results.Add([pscustomobject]@{
        step = $tag; kind = $kind; verdict = $verdict; miss_deg = [Math]::Round($miss, 2); impact = $impact
        score = [Math]::Round([double]$r.score, 0); fresh = $fresh; rewritten = $rew; turned_deg = $turned
        line_vs_shot_deg = [Math]::Round($lineAgree, 2); off_px = $offset; flash_deg = [Math]::Round($flashMiss, 2)
    })
    $script:lastShot = "$shot"
    Write-Host ("{0,-14} {1,-12} mark={2,6:F2} deg off_px={3,-9} flash={4,6:F2} deg impact={5,-10} score={6,-5:F0} fresh={7} rewritten={8} turned={9}" -f `
        $tag, $verdict, $miss, $offset, $flashMiss, $impact, [double]$r.score, $fresh, $rew, $turned)
}

$rng = New-Object System.Random
$script:jitter = @(($rng.NextDouble() * 6 - 3), ($rng.NextDouble() * 1.5 - 0.75))
Write-Host ("per-run offset: yaw {0:F2} deg, pitch {1:F2} deg" -f $script:jitter[0], $script:jitter[1])
[void](Focus-Game)
[void](Send-Aim @('ray off', 'shot aim'))
Start-Sleep -Seconds 1
Clear-WeaponIdle | Out-Null

try {
    Step 'null_a'   @{}
    # Each step a slightly different bore: a round sent down the same bore as an earlier step lands
    # in that step's mark and leaves no new one to find (roll does not move the bore at all).
    Step 'null_b'   @{ HandYaw = 16.5 }
    Step 'hand_r'   @{ HandYaw = 5; HandPitch = -8 }
    Step 'hand_l'   @{ HandYaw = 28; HandPitch = -4 }
    Step 'hand_up'  @{ HandYaw = 15; HandPitch = -2.5 }
    Step 'far'      @{ HandYaw = 20; HandPitch = -3 }
    Step 'head_l'   @{ HeadYaw = 15 }
    Step 'head_dn'  @{ HeadPitch = -15; HandPitch = -10 }
    Step 'roll'     @{ HandYaw = 13.5; HandRoll = 35 }
    Step 'near'     @{ HandPitch = -30; HeadPitch = -20 }
    if ($Only.Count -eq 0 -or $Only -contains 'turn') {
        [void](Send-Weapon @('turn 30'))
        Start-Sleep -Seconds 1.5
        Step 'turn' @{ HandYaw = 8; HandPitch = -6 }
        [void](Send-Weapon @('turn -30'))
        Start-Sleep -Seconds 1.5
    }
    # The game's own shot, with the character itself aimed at the ground (real mouse motion: the
    # servo corrects yaw only) and the head with it so the mark is in view. The controller is out of
    # frame above. `self`: against the game's own ray -- the instrument's calibration. `game_*`
    # (contrast): against the barrel line, pointed elsewhere -- it must miss by far, or the test
    # cannot tell the redirect from the game's shot and proves nothing.
    [void](Send-Aim @('shot observe'))
    Pitch-Character 6
    Start-Sleep -Seconds 1
    $up = @{ HeadPitch = -30; HandX = 0.70; HandY = 0.60; HandZ = 0.20 }
    Step 'self_near' $up 'self'
    Step 'game_r'   ($up + @{ HandYaw = -10; HandPitch = -25 }) 'contrast'
    Step 'game_l'   ($up + @{ HandYaw = 25; HandPitch = -20 }) 'contrast'
    Pitch-Character -3
    Start-Sleep -Seconds 1
    Step 'self_mid' @{ HeadPitch = -15; HandX = 0.70; HandY = 0.60; HandZ = 0.20 } 'self'
    Pitch-Character -3
    [void](Send-Aim @('shot aim'))

    if ($TwoHand) {
        # Two hands: the left takes the foregrip and steers the barrel about the right palm. The
        # round must follow the STEERED barrel, so besides landing on the fired ray, the fired ray
        # must have turned away from where the right controller points by the hold's own swing.
        $R = @{ HandX = 0.0; HandY = 0.10; HandZ = -0.15; HandYaw = 0; HandPitch = 0 }
        $restL = @{ LHandX = 0.05; LHandY = 0.08; LHandZ = -0.12 }
        $script:noJitter = $true
        $h = $null
        $rest = @{} + $R + $restL
        [void](Set-MockInput @rest)
        for ($i = 0; $i -lt 20; $i++) {
            Start-Sleep -Seconds 1
            $h = Get-Hold
            if ($h['fc_ok'] -eq '1' -and $h['fresh'] -eq '1' -and [double]::Parse($h['fc_len'], $inv) -ge 0.18) { break }
        }
        if ($h['fc_ok'] -ne '1' -or [double]::Parse($h['fc_len'], $inv) -lt 0.18) {
            Write-Host "two-handed: no long-gun foregrip measured (fc_ok=$($h['fc_ok']) fc_len=$($h['fc_len'])) -- skipped"
        } else {
            $fc = @($h['fc'] -split ',' | ForEach-Object { [double]::Parse($_, $inv) })
            Write-Host ("foregrip fc=({0:N3}, {1:N3}, {2:N3}) lever={3}" -f $fc[0], $fc[1], $fc[2], $h['fc_len'])
            function Left-On([double]$df = 0, [double]$dl = 0, [double]$du = 0) {
                $p = @{} + $R
                $p['LHandX'] = $R.HandX + 0.4 - ($fc[1] + $dl)
                $p['LHandY'] = $R.HandY + ($fc[2] + $du)
                $p['LHandZ'] = $R.HandZ - ($fc[0] + $df)
                $p['GripL'] = 1
                return $p
            }
            $grab = Left-On
            [void](Set-MockInput @grab)
            Start-Sleep -Seconds 1.5
            foreach ($case in @(@{ n = 'two_down'; dl = 0.0; du = -0.08 }, @{ n = 'two_down_l'; dl = 0.07; du = -0.08 })) {
                $pose = Left-On 0 $case.dl $case.du
                Step $case.n $pose 'barrel'
                $hold = Get-Hold
                $ctrl = Get-Vec "$(Get-VrLog 'ev=vr\.arms where side=R' 1)" 'ctrl_fwd'
                $fwd = Get-Vec $script:lastShot 'new_fwd'
                if ($ctrl -and $fwd) {
                    $n = [Math]::Sqrt($ctrl[0] * $ctrl[0] + $ctrl[1] * $ctrl[1] + $ctrl[2] * $ctrl[2])
                    $dot = ($ctrl[0] * $fwd[0] + $ctrl[1] * $fwd[1] + $ctrl[2] * $fwd[2]) / $n
                    $steer = [Math]::Acos([Math]::Min(1.0, $dot)) * 180 / [Math]::PI
                    Write-Host ("  {0}: state={1} swing_deg={2}; fired ray vs right controller {3:F1} deg" -f $case.n, $hold['state'], $hold['swing_deg'], $steer)
                }
            }
            $free = @{} + $R + $restL
            [void](Set-MockInput @free)
            Start-Sleep -Seconds 1
        }
        $script:noJitter = $false
    }
} finally {
    Pitch-Character (-$script:pitched)
    [void](Send-Aim @('shot aim'))
    [void](Set-MockInput @base)
}
Write-Host ''
$results | Format-Table -AutoSize | Out-String -Width 220 | Write-Host
foreach ($k in 'barrel', 'contrast', 'self') {
    $rows = @($results | Where-Object { $_.kind -eq $k })
    if ($rows.Count -eq 0) { continue }
    $measured = @($rows | Where-Object { $_.verdict -ne 'INCONCLUSIVE' })
    $okRows = @($rows | Where-Object { $_.verdict -eq 'OK' })
    $worst = if ($k -ne 'contrast' -and $okRows.Count) { ($okRows | Measure-Object miss_deg -Maximum).Maximum } else { $null }
    Write-Host ("{0,-9} {1}/{2} measured, {3} OK, {4} FAIL{5}" -f $k, $measured.Count, $rows.Count, $okRows.Count,
        ($measured.Count - $okRows.Count), $(if ($worst -ne $null) { ", worst {0:F2} deg" -f $worst } else { '' }))
}
Write-Host ("bars: barrel within {0} deg of the fired ray; contrast beyond {1} deg of the barrel; self within {2} deg (the game's hip-fire spread)" -f $PassDeg, (3 * $PassDeg), $SelfDeg)
