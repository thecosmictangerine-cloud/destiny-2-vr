# The two-handed hold on the mock, in a running session (a first-person area, F9 on, arms ready).
#
# Every step sets both controllers, waits, asks the DLL for its hold line (`twohand dump`) and checks
# it against what the geometry says must happen, then takes a screenshot with the overlay on: the
# handguard on the drawn gun (cyan free, amber taking hold, green held) with the left palm on it,
# and a magenta line from the left controller to that palm -- which a held foregrip has none of.
#
# The numbers that decide it:
#   line_err  distance from the left controller's palm to the handguard line of the DRAWN gun. With
#             two-handed aim the gun is turned until that line runs through the palm, so this is the
#             whole claim in one number. It is measured from the GPU palette, i.e. from what was drawn.
#   hand_err  distance from the left controller's palm to where the left hand was drawn.
#   swing_deg how far the left hand turned the gun away from where the right controller points.
#
# The foregrip is learned from the game's animation, so its position is read back from the log
# (`fc`, in the right controller's forward/LEFT/up frame) and the left controller is put on it. With
# both controllers in neutral orientation that is: LHandX = HandX + 0.4 - left, LHandY = HandY + up,
# LHandZ = HandZ - fwd (the mock's hands rest at x = +-0.2, and its stage is +X right, -Z forward).
param(
    [string]$Out = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'build\shots\twohand'),
    [double]$Settle = 1.0,
    [switch]$NoShots,
    [switch]$SkipSwap,
    # The mapping before physical ADS: left trigger = aim, left grip contextual (grenade away from the gun).
    [switch]$ButtonMode
)
$ErrorActionPreference = 'Stop'
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'lib\GameIO.ps1')
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'lib\VrCommands.ps1')
if (-not (Test-Path $Out)) { New-Item -ItemType Directory -Force -Path $Out | Out-Null }
$mockLog = 'C:\Games\Sunrise\SVR_MockXR.log'
$inv = [System.Globalization.CultureInfo]::InvariantCulture

# The raised baseline Test-Arms uses: the mock's hands otherwise sit at the bottom edge of the view.
$R = @{ HandX = 0.0; HandY = 0.10; HandZ = -0.15 }
$restL = @{ LHandX = 0.05; LHandY = 0.08; LHandZ = -0.12 }

function Get-Hold {
    [void](Send-Arms @('twohand dump'))
    Start-Sleep -Milliseconds 350
    $line = Get-VrLog 'ev=vr\.arms hold state=' 1
    $h = @{}
    foreach ($m in [regex]::Matches("$line", '(\w+)=([^\s]+)')) { $h[$m.Groups[1].Value] = $m.Groups[2].Value }
    return $h
}
function Num([hashtable]$h, [string]$k) { return [double]::Parse($h[$k], $inv) }
# The foregrip keeps settling for a second or two after a swap, while the seat loops bring the new
# gun onto the controller. Placing the hand from an early reading puts it a centimetre off the line
# and the gun then (correctly) turns a couple of degrees to meet it, which reads as a failure of the
# slide test. So: two readings a second apart that agree to 2 mm.
function Wait-StableFc([double]$MinLever = 0, [int]$TimeoutSeconds = 15) {
    $prev = $null
    for ($i = 0; $i -lt $TimeoutSeconds; $i++) {
        Start-Sleep -Seconds 1
        $h = Get-Hold
        if ($h['fc_ok'] -ne '1' -or $h['fresh'] -ne '1' -or (Num $h 'fc_len') -lt $MinLever) { $prev = $null; continue }
        $fc = Get-Fc $h
        if ($prev) {
            $d = [Math]::Sqrt([Math]::Pow($fc[0] - $prev[0], 2) + [Math]::Pow($fc[1] - $prev[1], 2) + [Math]::Pow($fc[2] - $prev[2], 2))
            if ($d -lt 0.002) { return $h }
        }
        $prev = $fc
    }
    return $h
}
function Get-Fc([hashtable]$h) {
    $p = $h['fc'] -split ','
    return @([double]::Parse($p[0], $inv), [double]::Parse($p[1], $inv), [double]::Parse($p[2], $inv))
}
function Read-MockLog {
    $fs = [System.IO.File]::Open($mockLog, 'Open', 'Read', 'ReadWrite')
    try { $sr = New-Object System.IO.StreamReader($fs); return ($sr.ReadToEnd() -split "`n") } finally { $fs.Dispose() }
}
# Counted in the DLL's own log (`ev=vr.xr haptic`), which records what was sent and the runtime's answer.
function Count-Haptics { return @(Read-VrLog 20000 | Where-Object { $_ -match 'ev=vr\.xr haptic hand=L' }).Count }

# Left controller on the foregrip, plus an offset in the controller's (forward, left, up) frame.
function Left-On([double[]]$fc, [double]$df = 0, [double]$dl = 0, [double]$du = 0) {
    return @{
        LHandX = $R.HandX + 0.4 - ($fc[1] + $dl)
        LHandY = $R.HandY + ($fc[2] + $du)
        LHandZ = $R.HandZ - ($fc[0] + $df)
    }
}

$results = New-Object System.Collections.ArrayList
function Step([string]$name, [hashtable]$left, [hashtable]$extra, [scriptblock]$check) {
    $p = @{} + $R + $left
    if ($extra) { foreach ($k in $extra.Keys) { $p[$k] = $extra[$k] } }
    [void](Set-MockInput @p)
    Start-Sleep -Seconds $Settle
    if (-not $NoShots) { Save-Shot (Join-Path $Out "$name.png") | Out-Null }
    $h = Get-Hold
    $verdict = & $check $h
    $ok = $verdict -eq $true
    [void]$results.Add([pscustomobject]@{
        step = $name; ok = $ok; state = $h['state']; mode = $h['mode']; w = $h['w']; swing = $h['swing_deg']
        slide = $h['slide']; line_err = $h['line_err']; hand_err = $h['hand_err']; dist = $h['dist']; last = $h['last']
        note = if ($ok) { '' } else { "$verdict" }
    })
    Write-Host ("{0,-16} {1,-4} state={2,-9} mode={3,-7} w={4} swing={5} slide={6} line_err={7} hand_err={8} dist={9} last={10} {11}" -f `
        $name, $(if ($ok) { 'OK' } else { 'FAIL' }), $h['state'], $h['mode'], $h['w'], $h['swing_deg'], $h['slide'],
        $h['line_err'], $h['hand_err'], $h['dist'], $h['last'], $(if ($ok) { '' } else { "<- $verdict" }))
    return $h
}

# A left-trigger press as the gamepad classified it (`ev=vr.pad left_trigger`), after `trigger` runs.
function Get-Fresh-Pad([scriptblock]$trigger) {
    $before = @(Read-VrLog 4000 | Where-Object { $_ -match 'ev=vr\.pad left_trigger' }).Count
    & $trigger
    for ($i = 0; $i -lt 20; $i++) {
        Start-Sleep -Milliseconds 150
        $lines = @(Read-VrLog 4000 | Where-Object { $_ -match 'ev=vr\.pad left_trigger' })
        if ($lines.Count -gt $before) { return $lines[-1] }
    }
    return $null
}
function Record-Pad([string]$name, [string]$line, [string]$expect) {
    $ok = "$line" -match [regex]::Escape($expect)
    [void]$results.Add([pscustomobject]@{ step = $name; ok = $ok; note = if ($ok) { '' } else { "got: $line" } })
    Write-Host ("{0,-16} {1,-4} {2}" -f $name, $(if ($ok) { 'OK' } else { 'FAIL' }), $(if ($line) { $line.Substring($line.IndexOf('ev=')) } else { 'no press logged' }))
}

[void](Focus-Game)
[void](Send-Arms @('overlay on'))
[void](Send-Ads @($(if ($ButtonMode) { 'mode button' } else { 'mode auto' })))

# --- The foregrip has to be known first: rest pose, still, until the log says it was measured. ---
$p = @{} + $R + $restL
[void](Set-MockInput @p)
$h = $null
for ($i = 0; $i -lt 20; $i++) {
    Start-Sleep -Seconds 1
    $h = Get-Hold
    if ($h['fc_ok'] -eq '1' -and $h['fresh'] -eq '1') { break }
}
if ($h['fc_ok'] -ne '1') { throw "foregrip never measured: $($h | Out-String)" }

# Two-handed aim needs a long gun. Swap (Y) until the foregrip reads as one.
for ($i = 0; $i -lt 3 -and (Num $h 'fc_len') -lt 0.18; $i++) {
    Write-Output ("weapon in hand has a support grip (fc_len={0}); swapping" -f $h['fc_len'])
    $p = @{} + $R + $restL + @{ BtnY = 1 }; [void](Set-MockInput @p); Start-Sleep -Milliseconds 250
    $p = @{} + $R + $restL; [void](Set-MockInput @p)
    for ($j = 0; $j -lt 12; $j++) {
        Start-Sleep -Seconds 1
        $h = Get-Hold
        if ($h['fc_ok'] -eq '1' -and $h['fresh'] -eq '1' -and (Num $h 'fc_len') -ge 0.18) { break }
    }
}
$h = Wait-StableFc 0.18
$fc = Get-Fc $h
$lever = Num $h 'fc_len'
Write-Output ("foregrip fc=({0:N3}, {1:N3}, {2:N3}) lever={3:N3}" -f $fc[0], $fc[1], $fc[2], $lever)
$haptics0 = Count-Haptics

# --- Long gun: take hold, aim with the left hand, slide, let go. ---
$tol = 0.006   # 6 mm: the drawn line through the controller's palm
[void](Step 'a_near_free' (Left-On $fc) $null { param($h)
    if ($h['state'] -ne 'free') { "state $($h['state'])" } elseif ((Num $h 'dist') -gt 0.02) { "dist $($h['dist'])" } else { $true } })
[void](Step 'b_grab' (Left-On $fc) @{ GripL = 1 } { param($h)
    if ($h['state'] -ne 'held' -or $h['mode'] -ne 'aim') { "state $($h['state']) mode $($h['mode'])" }
    elseif ((Num $h 'w') -lt 0.999) { "w $($h['w'])" }
    elseif ((Num $h 'line_err') -gt $tol) { "line_err $($h['line_err'])" }
    elseif ((Num $h 'hand_err') -gt 0.03) { "hand_err $($h['hand_err'])" }
    else { $true } })
$haptics1 = Count-Haptics
$swingExpected = [Math]::Atan2(0.08, $lever) * 180 / [Math]::PI
[void](Step 'c_swing_left' (Left-On $fc 0 0.08 0) @{ GripL = 1 } { param($h)
    if ($h['state'] -ne 'held') { "state $($h['state'])" }
    elseif ([Math]::Abs((Num $h 'swing_deg') - $swingExpected) -gt 2.5) { "swing $($h['swing_deg']) expected ~$([Math]::Round($swingExpected,1))" }
    elseif ((Num $h 'line_err') -gt $tol) { "line_err $($h['line_err'])" }
    else { $true } })
[void](Step 'd_swing_up' (Left-On $fc 0 0 0.08) @{ GripL = 1 } { param($h)
    if ($h['state'] -ne 'held') { "state $($h['state'])" }
    elseif ([Math]::Abs((Num $h 'swing_deg') - $swingExpected) -gt 2.5) { "swing $($h['swing_deg']) expected ~$([Math]::Round($swingExpected,1))" }
    elseif ((Num $h 'line_err') -gt $tol) { "line_err $($h['line_err'])" }
    else { $true } })
[void](Step 'e_slide_fwd' (Left-On $fc 0.06 0 0) @{ GripL = 1 } { param($h)
    if ($h['state'] -ne 'held') { "state $($h['state'])" }
    elseif ([Math]::Abs((Num $h 'slide') - 0.06) -gt 0.01) { "slide $($h['slide'])" }
    elseif ((Num $h 'swing_deg') -gt 1.5) { "swing $($h['swing_deg']) (sliding must not turn the gun)" }
    elseif ((Num $h 'line_err') -gt $tol) { "line_err $($h['line_err'])" }
    else { $true } })
[void](Step 'f_right_roll' (Left-On $fc 0 0.05 0) @{ GripL = 1; HandRoll = 35 } { param($h)
    if ($h['state'] -ne 'held') { "state $($h['state'])" }
    elseif ((Num $h 'line_err') -gt $tol) { "line_err $($h['line_err'])" }
    else { $true } })
# Artificial turning and head motion must not move the gun relative to the hands: both hands are
# folded by the same room anchor, so the hold is a function of their difference alone.
[void](Send-Weapon @('turn 90'))
[void](Step 'f2_turned_90' (Left-On $fc 0 0.05 0) @{ GripL = 1 } { param($h)
    if ($h['state'] -ne 'held') { "state $($h['state'])" }
    elseif ((Num $h 'line_err') -gt $tol) { "line_err $($h['line_err'])" }
    else { $true } })
[void](Send-Weapon @('turn -90'))
[void](Step 'f3_head_lean' (Left-On $fc 0 0.05 0) @{ GripL = 1; HeadX = 0.10; HeadZ = -0.05; HeadYaw = 20; HeadPitch = -10 } { param($h)
    if ($h['state'] -ne 'held') { "state $($h['state'])" }
    elseif ((Num $h 'line_err') -gt $tol) { "line_err $($h['line_err'])" }
    else { $true } })
[void](Step 'g_slide_off' (Left-On $fc 0.30 0 0) @{ GripL = 1 } { param($h)
    if ($h['state'] -ne 'pending' -or $h['last'] -ne 'slide') { "state $($h['state']) last $($h['last'])" } else { $true } })
[void](Step 'h_regrab' (Left-On $fc) @{ GripL = 1 } { param($h)
    if ($h['state'] -ne 'held') { "state $($h['state'])" } elseif ((Num $h 'line_err') -gt $tol) { "line_err $($h['line_err'])" } else { $true } })
[void](Step 'i_fire_held' (Left-On $fc) @{ GripL = 1; TrigR = 1 } { param($h)
    if ($h['state'] -ne 'held') { "state $($h['state'])" } else { $true } })
[void](Step 'j_release' (Left-On $fc) $null { param($h)
    if ($h['state'] -ne 'free' -or (Num $h 'w') -gt 0.0) { "state $($h['state']) w $($h['w'])" } else { $true } })

# --- What a squeeze means depends on where it starts. ---
[void](Step 'k_pending' (Left-On $fc 0 0 0.18) @{ GripL = 1 } { param($h)
    if ($h['state'] -ne 'pending') { "state $($h['state'])" } else { $true } })
[void](Step 'l_arrive' (Left-On $fc) @{ GripL = 1 } { param($h)
    if ($h['state'] -ne 'held') { "state $($h['state'])" } else { $true } })
[void](Step 'm_open' (Left-On $fc) $null { param($h) if ($h['state'] -ne 'free') { "state $($h['state'])" } else { $true } })
$grenades0 = [int](Get-Hold)['grenades']
if ($ButtonMode) {
    # The old mapping (`ads mode button`): a squeeze away from the gun is the grenade, for as long as it lasts.
    [void](Step 'n_grenade' $restL @{ GripL = 1 } { param($h)
        if ($h['state'] -ne 'grenade') { "state $($h['state'])" } elseif ([int]$h['grenades'] -ne $grenades0 + 1) { "grenades $($h['grenades'])" } else { $true } })
    [void](Step 'o_grenade_stays' (Left-On $fc) @{ GripL = 1 } { param($h)
        if ($h['state'] -ne 'grenade') { "state $($h['state']) (a grenade squeeze must not turn into a hold)" } else { $true } })
} else {
    # Physical ADS (the default): the grip is only the hold. A squeeze away from the gun waits, and
    # takes hold when the hand reaches the handguard; the grenade is the left trigger.
    [void](Step 'n_grip_away' $restL @{ GripL = 1 } { param($h)
        if ($h['state'] -ne 'pending') { "state $($h['state'])" } elseif ([int]$h['grenades'] -ne $grenades0) { "grenades $($h['grenades'])" } else { $true } })
    [void](Step 'o_grip_arrives' (Left-On $fc) @{ GripL = 1 } { param($h)
        if ($h['state'] -ne 'held') { "state $($h['state']) (a squeeze carried to the handguard must take hold)" } else { $true } })
    # The left trigger while holding is the grip tightening, not a throw; free, it throws.
    $pressHeld = Get-Fresh-Pad { $p = @{} + $R + (Left-On $fc) + @{ GripL = 1; TrigL = 1 }; [void](Set-MockInput @p) }
    $p = @{} + $R + (Left-On $fc) + @{ GripL = 1 }; [void](Set-MockInput @p); Start-Sleep -Milliseconds 400
    Record-Pad 'o2_trigger_held' $pressHeld 'action=ignored'
    $p = @{} + $R + $restL; [void](Set-MockInput @p); Start-Sleep -Milliseconds 600
    $pressFree = Get-Fresh-Pad { $p = @{} + $R + $restL + @{ TrigL = 1 }; [void](Set-MockInput @p) }
    $p = @{} + $R + $restL; [void](Set-MockInput @p); Start-Sleep -Milliseconds 1500
    Record-Pad 'o3_trigger_free' $pressFree 'action=grenade'
}
[void](Step 'p_open' $restL $null { param($h) if ($h['state'] -ne 'free') { "state $($h['state'])" } else { $true } })

# --- A swap while holding: the hold is suspended and the next weapon's foregrip learned. ---
if (-not $SkipSwap) {
    [void](Step 'q_hold_again' (Left-On $fc) @{ GripL = 1 } { param($h) if ($h['state'] -ne 'held') { "state $($h['state'])" } else { $true } })
    $p = @{} + $R + (Left-On $fc) + @{ GripL = 1; BtnY = 1 }; [void](Set-MockInput @p); Start-Sleep -Milliseconds 250
    [void](Step 'r_swapped' (Left-On $fc) @{ GripL = 1 } { param($h)
        if ($h['state'] -ne 'suspended' -or $h['last'] -ne 'swap') { "state $($h['state']) last $($h['last'])" } else { $true } })
    $h2 = Wait-StableFc
    $fc2 = Get-Fc $h2
    Write-Output ("after swap: fc=({0:N3}, {1:N3}, {2:N3}) lever={3} state={4}" -f $fc2[0], $fc2[1], $fc2[2], $h2['fc_len'], $h2['state'])
    $support = (Num $h2 'fc_len') -lt 0.18
    [void](Step 's_second_weapon' (Left-On $fc2) @{ GripL = 1 } { param($h)
        if ($h['state'] -ne 'held') { "state $($h['state'])" }
        elseif ($support -and $h['mode'] -ne 'support') { "mode $($h['mode']) for lever $($h['fc_len'])" }
        elseif ((-not $support) -and (Num $h 'line_err') -gt $tol) { "line_err $($h['line_err'])" }
        else { $true } })
    if ($support) {
        [void](Step 't_support_stray' (Left-On $fc2 0 0 0.14) @{ GripL = 1 } { param($h)
            if ($h['state'] -ne 'held') { "state $($h['state']) (0.14 m is inside the release radius)" } else { $true } })
        [void](Step 'u_support_off' (Left-On $fc2 0 0 0.24) @{ GripL = 1 } { param($h)
            if ($h['state'] -ne 'pending' -or $h['last'] -ne 'range') { "state $($h['state']) last $($h['last'])" } else { $true } })
    }
    [void](Step 'v_open' $restL $null { param($h) if ($h['state'] -ne 'free') { "state $($h['state'])" } else { $true } })
    # And back to the first weapon, so the next run starts where this one did.
    $p = @{} + $R + $restL + @{ BtnY = 1 }; [void](Set-MockInput @p); Start-Sleep -Milliseconds 250
}
$p = @{} + $R + $restL
[void](Set-MockInput @p)

$haptics = (Count-Haptics) - $haptics0
Write-Output ("haptic pulses on the left controller during the run: {0} (first grab produced {1})" -f $haptics, ($haptics1 - $haptics0))
$failed = @($results | Where-Object { -not $_.ok })
Write-Output ("{0} steps, {1} failed" -f $results.Count, $failed.Count)
$results | Export-Csv -NoTypeInformation -Path (Join-Path $Out 'results.csv')
