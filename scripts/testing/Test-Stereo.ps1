# What the headset shows, verified on the mock: the eye-shaped mono image and alternating-eye stereo
# (docs/DISPLAY.md). Every check reads each eye's image as the compositor builds it (the mock's
# emulated compositor, eye_view.py) or the frame ledger's trace (frame_trace.py), never the desktop.
#
# Needs the game on the mock, in a world, F9 on (Run-Patrol.ps1), with the mock's Quest 3 geometry:
# SVR_MockHmd.txt = "1824 1968 -54 40 44 -55 90 0.062".
#
#   Test-Stereo.ps1                 # both modes
#   Test-Stereo.ps1 -Only mono      # or stereo
#   Test-Stereo.ps1 -Compositor meta  # every composite as Meta's PC runtime is reported to compose
#
# -Compositor meta makes the mock ignore each view's declared FOV (the sub-image rect is mapped onto
# the eye's own FOV) and its declared pose (the image is taken as drawn from the eye): the Quest 3
# double vision of 2026-10-16. Whatever the compositor, the mono block also composes one still frame
# both ways and requires the two to agree to a fraction of a pixel ("runtime-independent").
param(
    [ValidateSet('all', 'mono', 'stereo')][string]$Only = 'all',
    # The render density checked, and set first: the module's default is 0.8 (vr_display.cpp g_scale).
    [double]$Scale = 0.8,
    [ValidateSet('spec', 'meta')][string]$Compositor = 'spec',
    [switch]$KeepStereo
)
$script:metaWords = @('fovlock', 'poselock')
$script:compWords = if ($Compositor -eq 'meta') { $script:metaWords } else { @() }
$ErrorActionPreference = 'Stop'
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'lib\GameIO.ps1')
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$out = Join-Path $root 'build\shots\stereo'
if (-not (Test-Path $out)) { New-Item -ItemType Directory -Force -Path $out | Out-Null }
$game = 'C:\Games\Sunrise'
$py = Join-Path $PSScriptRoot 'eye_view.py'
$enc = New-Object System.Text.UTF8Encoding($false)
$script:results = @()
$inv = [System.Globalization.CultureInfo]::InvariantCulture

function Check([string]$Name, [bool]$Pass, [string]$Detail) {
    $script:results += [pscustomobject]@{ Check = $Name; Result = $(if ($Pass) { 'PASS' } else { 'FAIL' }); Detail = $Detail }
    Write-Output ('{0,-4} {1,-34} {2}' -f $(if ($Pass) { 'ok' } else { 'FAIL' }), $Name, $Detail)
}
function F([double]$v, [string]$fmt = '0.000') { $v.ToString($fmt, $inv) }

function Send-Display([string[]]$Lines) {
    $path = Join-Path $game 'SVR_Display.txt'
    [System.IO.File]::WriteAllText($path, (($Lines -join "`n") + "`n"), $enc)
    $deadline = (Get-Date).AddSeconds(5)
    while ((Test-Path $path) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 100 }
}

function Pose([double]$Yaw = 0, [double]$TurnX = 0, [double]$HeadX = 0) {
    [void](Set-MockInput -HeadYaw $Yaw -HeadX $HeadX -HandY 0.12 -TurnX $TurnX)
}

<# Composes and returns the file tags (one, or the sequence's). #>
# -As spec|meta overrides the battery's -Compositor for one composite.
function Compose([string]$Tag, [switch]$World, [int]$Seq = 0, [int]$TimeoutSeconds = 30, [string]$As = '') {
    Get-ChildItem $game -Filter "SVR_seen_$Tag*" -ErrorAction SilentlyContinue | Remove-Item -Force
    $words = @($Tag)
    $words += $(switch ($As) { 'spec' { @() } 'meta' { $script:metaWords } default { $script:compWords } })
    if ($World) { $words += 'world' }
    if ($Seq -gt 1) { $words += @('seq', $Seq) }
    [System.IO.File]::WriteAllText((Join-Path $game 'SVR_MockCompose.txt'), (($words -join ' ') + "`n"), $enc)
    $last = if ($Seq -gt 1) { '{0}_f{1:D2}' -f $Tag, ($Seq - 1) } else { $Tag }
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while (-not (Test-Path (Join-Path $game "SVR_seen_$last.txt"))) {
        if ((Get-Date) -gt $deadline) { throw "no composite for $Tag" }
        Start-Sleep -Milliseconds 100
    }
    Start-Sleep -Milliseconds 300
    Get-ChildItem $game -Filter "SVR_seen_$Tag*" | Copy-Item -Destination $out -Force
    if ($Seq -gt 1) { return (0..($Seq - 1) | ForEach-Object { '{0}_f{1:D2}' -f $Tag, $_ }) }
    return @($Tag)
}

function Analyse([string]$Tag) {
    $json = & python $py $out $Tag --out $out --json
    return ($json | Select-Object -Last 1 | ConvertFrom-Json)
}
function Get-PairDiff([string]$A, [string]$B, [string]$Region = 'centre') {
    $json = & python $py diff $out $A $B $Region --out $out
    return (($json -join "`n") | ConvertFrom-Json)
}
function Get-Sequence([string]$Tag, [int]$N) {
    return (& python $py seq $out $Tag $N | ForEach-Object { $_ | ConvertFrom-Json })
}

<#
Moves the head on a sine while `Body` runs, so composites see motion. The motion is a function of TIME
inside the mock (SVR_MockMotion.txt): xrLocateViews answers for the instant asked and the compositor
knows where the eye is when each frame is displayed, as a headset does. (It used to be a job rewriting
the input file every 12 ms: the head then jumped between writes and stood still in between, and a
pose located for the wrong instant could not be told from the right one.)
#>
function With-HeadMotion([scriptblock]$Body, [double]$Amplitude = 10, [double]$Rate = 2.5) {
    $motion = Join-Path $game 'SVR_MockMotion.txt'
    [System.IO.File]::WriteAllText($motion, ("yaw {0} {1}`n" -f (F $Amplitude '0.###'), (F $Rate '0.###')), $enc)
    try {
        Start-Sleep -Milliseconds 1500
        & $Body
    } finally {
        Remove-Item $motion -ErrorAction SilentlyContinue
        Start-Sleep -Milliseconds 300
        Pose
    }
}

if (-not (Get-GameProcess)) { throw 'the game is not running' }
[void](Assert-GameFocus)
Send-Display @("scale $(F $Scale)")
Start-Sleep -Seconds 6

# ------------------------------------------------------------------------------------------------
if ($Only -ne 'stereo') {
    Write-Output '== mono, eye-shaped'
    Send-Display @('stereo off', 'shape eye')
    Pose
    Start-Sleep -Seconds 4
    $m = Analyse (@(Compose 'mono')[0])
    foreach ($e in 'eye0', 'eye1') {
        $s = $m.$e
        $bands = @($s.bands_deg.left, $s.bands_deg.right, $s.bands_deg.top, $s.bands_deg.bottom) | Measure-Object -Maximum
        Check "$e fills its field" ($s.coverage -ge 0.99 -and $bands.Maximum -le 0.5) ("coverage {0}  widest band {1} deg" -f (F $s.coverage), (F $bands.Maximum '0.00'))
        $black = @($s.black_deg.left, $s.black_deg.right, $s.black_deg.top, $s.black_deg.bottom) | Measure-Object -Maximum
        Check "$e no black band in the image" ($black.Maximum -le 0.5) ("widest black edge {0} deg (top {1}, bottom {2})" -f (F $black.Maximum '0.00'), $s.black_deg.top, $s.black_deg.bottom)
        Check "$e square pixels" ([Math]::Abs($s.density.stretch - 1.0) -le 0.02) ("stretch {0}" -f (F $s.density.stretch))
        Check "$e density" ([Math]::Abs($s.density.h - $Scale) -le 0.03 * $Scale -and [Math]::Abs($s.density.v - $Scale) -le 0.03 * $Scale) ("source px per panel px h {0} v {1} (scale {2})" -f (F $s.density.h), (F $s.density.v), (F $Scale '0.00'))
    }
    $h = $m.h_disparity_deg; $v = $m.v_disparity_deg
    Check 'mono: no disparity' ([Math]::Abs($h.median) -le 0.02 -and $h.divergent_fraction -le 0.002 -and $h.near_fraction -le 0.002) ("median {0}  divergent {1}  near {2}" -f $h.median, $h.divergent_fraction, $h.near_fraction)
    Check 'mono: no vertical disparity' ($v.abs_p95 -le 0.03) ("|p95| {0} deg" -f $v.abs_p95)

    # Runtime-independent: one still frame composed as the spec says and as Meta's runtime is
    # reported to (declared FOV and pose ignored). The submission must not depend on which is true:
    # every point at the same angle in each eye, the identity to a fraction of a pixel.
    $indSpec = @(Compose 'ind_spec' -World -As spec)[0]
    $indMeta = @(Compose 'ind_meta' -World -As meta)[0]
    $ind = (& python $py fidelity $out $indSpec $indMeta --out $out | Select-Object -Last 1 | ConvertFrom-Json)
    foreach ($e in 'eye0', 'eye1') {
        $x = $ind.$e
        $ok = $x.matches -ge 30 -and [Math]::Abs($x.scale_x - 1) -le 0.005 -and [Math]::Abs($x.scale_y - 1) -le 0.005 `
            -and [Math]::Abs($x.shift_deg[0]) -le 0.05 -and [Math]::Abs($x.shift_deg[1]) -le 0.05
        Check "$e runtime-independent (spec vs Meta)" $ok ("scale {0} x {1}, shift {2},{3} deg, {4} matches, resid {5} px" -f $x.scale_x, $x.scale_y, $x.shift_deg[0], $x.shift_deg[1], $x.matches, $x.resid_px_median)
    }

    # Angular fidelity: the eye-shaped image against the old desktop-shaped one, whose layer declares
    # the projection the GPU received. Every distant point must sit at the same angle in the eye: the
    # identity, to a fraction of a pixel. Catches a squeezed band, a wrong declared FOV, a wrong pose.
    Send-Display @('shape desktop'); Start-Sleep -Seconds 8
    # The reference is always composed as the spec says: a desktop-shaped image cannot hold the eye's
    # vertical FOV, so a compositor that ignores the declared FOV would stretch the reference itself.
    $ref = @(Compose 'fid_desk' -World -As spec)[0]
    Send-Display @('shape eye'); Start-Sleep -Seconds 8
    $tst = @(Compose 'fid_eye' -World)[0]
    $fid = (& python $py fidelity $out $ref $tst --out $out | Select-Object -Last 1 | ConvertFrom-Json)
    # Only meaningful if the reference really was the desktop shape: a resize that silently failed
    # compares the eye shape with itself and passes by construction (it did once: a view of the back
    # buffer held by an overlay made DXGI refuse every resize).
    $refCov = (Analyse $ref).eye0.coverage
    Check 'fidelity reference is desktop-shaped' ($refCov -lt 0.9) ("reference coverage {0} (desktop shape ~0.65)" -f (F $refCov))
    foreach ($e in 'eye0', 'eye1') {
        $x = $fid.$e
        $ok = $refCov -lt 0.9 -and $x.matches -ge 30 -and [Math]::Abs($x.scale_x - 1) -le 0.005 -and [Math]::Abs($x.scale_y - 1) -le 0.005 `
            -and [Math]::Abs($x.shift_deg[0]) -le 0.05 -and [Math]::Abs($x.shift_deg[1]) -le 0.05
        Check "$e angles exact (eye vs desktop shape)" $ok ("scale {0} x {1}, shift {2},{3} deg, {4} matches, resid {5} px" -f $x.scale_x, $x.scale_y, $x.shift_deg[0], $x.shift_deg[1], $x.matches, $x.resid_px_median)
    }

    # Swim, static: the eye held still in the world, the head turned between the two composites.
    $a = @(Compose 'mswimA' -World)[0]
    Pose -Yaw 25; Start-Sleep -Seconds 3
    $b = @(Compose 'mswimB' -World)[0]
    Pose; Start-Sleep -Seconds 2
    $d = Get-PairDiff $a $b
    $sh = [Math]::Max([Math]::Abs($d.eye0.shift_px[0]), [Math]::Abs($d.eye0.shift_px[1]))
    Check 'mono: world stays put (head turned)' ($sh -le 0.5) ("shift {0} px (response {1})" -f (F $sh '0.00'), $d.eye0.response)

    # Swim, moving: consecutive frames with the head turning all the time.
    $tags = With-HeadMotion { Compose 'mswimseq' -World -Seq 8 }
    $worst = 0.0
    for ($i = 1; $i -lt $tags.Count; $i++) {
        $d = Get-PairDiff $tags[$i - 1] $tags[$i]
        $worst = [Math]::Max($worst, [Math]::Max([Math]::Abs($d.eye0.shift_px[0]), [Math]::Abs($d.eye0.shift_px[1])))
    }
    Check 'mono: world stays put (head moving)' ($worst -le 1.0) ("worst frame-to-frame shift {0} px" -f (F $worst '0.00'))
}

# ------------------------------------------------------------------------------------------------
if ($Only -ne 'mono') {
    Write-Output '== alternating-eye stereo'
    Send-Display @('stereo on', 'shape eye')
    Pose
    Start-Sleep -Seconds 4
    $tags = Compose 'st' -Seq 8
    $rows = Get-Sequence 'st' 8
    # The first row has nothing before it, so it always reads as both eyes fresh: judge from the second.
    $fresh = @($rows | Select-Object -Skip 1 | ForEach-Object { if ($_.e0.fresh -and -not $_.e1.fresh) { 'L' } elseif ($_.e1.fresh -and -not $_.e0.fresh) { 'R' } elseif ($_.e0.fresh -and $_.e1.fresh) { 'B' } else { '-' } }) -join ''
    $alternates = ($fresh -match '^(LR|RL)+L?R?$')
    Check 'stereo: eyes alternate' $alternates "fresh per frame (after the first): $fresh"
    $s = Analyse $tags[$tags.Count - 1]
    foreach ($e in 'eye0', 'eye1') {
        $x = $s.$e
        $black = @($x.black_deg.left, $x.black_deg.right, $x.black_deg.top, $x.black_deg.bottom) | Measure-Object -Maximum
        Check "$e fills its field (stereo)" ($x.coverage -ge 0.99 -and $black.Maximum -le 0.5) ("coverage {0}, widest black edge {1} deg" -f (F $x.coverage), (F $black.Maximum '0.00'))
    }
    # Disparity by whole-tile correlation (eye_view.py tile_match): SGBM cannot hold a dark gun 12 deg
    # nearer than the ground behind it, and DIS flow turns its edges into false vertical motion.
    $tn = $s.tile_disparity_deg.near; $tf = $s.tile_disparity_deg.far
    Check 'stereo: nothing beyond infinity' ($tf.divergent -le [Math]::Max(2, 0.01 * $tf.tiles)) ("{0} of {1} far tiles divergent" -f $tf.divergent, $tf.tiles)
    Check 'stereo: the world is far' ($tf.h_median -ge -0.05 -and $tf.h_median -le 1.0) ("far tiles median {0} deg (p90 {1}: the ground at the feet)" -f $tf.h_median, $tf.h_p90)
    Check 'stereo: no vertical disparity (world)' ($tf.v_abs_p95 -le 0.05) ("far tiles |p95| {0} deg" -f $tf.v_abs_p95)
    # The gun, at the controller ~0.25-0.5 m in front: ipd / depth = 7-14 deg, crossed.
    $gunOk = $null -ne $tn -and $tn.h_median -ge 5 -and $tn.h_median -le 16
    Check 'stereo: the gun is near' $gunOk $(if ($tn) { "near tiles median {0} deg = {1} m, over {2} tiles" -f $tn.h_median, $s.tile_disparity_deg.near_depth_m, $tn.tiles } else { 'no near tiles' })
    # The near objects, per side, by the median of their tiles (eye_view.py near_objects_vertical):
    # a single tile on a 30 cm object cannot be trusted vertically, a systematic offset moves them all.
    $nv = @($s.near_objects_vertical)
    # A side is judged only with enough tiles for a statistic (12), on the trimmed mean (20 % off each
    # end): with 9 tiles one stray match moved the median to 0.17 deg while the trimmed mean read 0.05.
    $judged = @($nv | Where-Object { $_.tiles -ge 12 })
    $worstV = ($judged | ForEach-Object { [Math]::Abs($_.v_trimmed_mean) } | Measure-Object -Maximum).Maximum
    Check 'stereo: no vertical disparity (hands, gun)' ($judged.Count -ge 1 -and $worstV -le 0.1) (($nv | ForEach-Object { "{0}: trimmed {1}, median {2} deg over {3} tiles{4}" -f $_.side, $_.v_trimmed_mean, $_.v_median, $_.tiles, $(if ($_.tiles -lt 12) { ' (too few to judge)' } else { '' }) }) -join '; ')

    # The drawn hand must be where the controller is IN DEPTH -- the one thing mono can never show
    # and stereo shows at once. The arm module's overlay (world_lines, at the controller's true
    # camera-relative position) is the instrument's ground truth; the glove's tiles around it must
    # agree, near and with the arm pushed 20 cm further out.
    $hd = @{}
    foreach ($z in 0, -0.2) {
        [void](Set-MockInput -HandY 0.12 -HandZ $z)
        Start-Sleep -Seconds 8
        $k = [int][Math]::Round(-$z * 100)
        [System.IO.File]::WriteAllText((Join-Path $game 'SVR_Arms.txt'), "overlay on`n", $enc); Start-Sleep -Seconds 2
        $on = @(Compose "hdon_$k" -Seq 2)[1]
        [System.IO.File]::WriteAllText((Join-Path $game 'SVR_Arms.txt'), "overlay off`n", $enc); Start-Sleep -Seconds 2
        $offT = @(Compose "hdoff_$k" -Seq 2)[1]
        $hd[$k] = (& python $py handdepth $out $on $offT | Select-Object -Last 1 | ConvertFrom-Json)
        $r = $hd[$k]
        $ok = $null -ne $r.hand_depth_m -and $null -ne $r.controller_depth_m -and [Math]::Abs($r.hand_depth_m - $r.controller_depth_m) -le 0.04
        Check ("stereo: hand at the controller's depth ({0} cm out)" -f $k) $ok ("drawn hand {0} m (p10-p90 {1}), controller {2} m, {3} tiles" -f $r.hand_depth_m, ($r.hand_depth_p10_p90 -join '-'), $r.controller_depth_m, $r.hand_tiles)
    }
    Pose; Start-Sleep -Seconds 2
    if ($hd[0].hand_depth_m -and $hd[20].hand_depth_m) {
        $moved = $hd[20].hand_depth_m - $hd[0].hand_depth_m
        Check 'stereo: hand depth follows the controller' ([Math]::Abs($moved - 0.2) -le 0.06) ("moved {0} m for 0.20 m (overlay: {1} m)" -f (F $moved), (F ($hd[20].controller_depth_m - $hd[0].controller_depth_m)))
    }

    # Each eye's image must be that eye's alone: an engine effect that accumulates over frames (a
    # temporal filter) would blend the other eye's view in. The stereo left image, compared with a
    # MONO image rendered from the left eye at the same pose, must differ no more than two mono
    # images taken the same seconds apart (the null pair: idle animation, lighting drift).
    $null = Compose 'own_s' -World -Seq 2
    # whichever of the two frames carried a fresh LEFT image is the one to compare
    $rowsOwn = Get-Sequence 'own_s' 2
    $ownS = if ($rowsOwn[1].e0.fresh) { 'own_s_f01' } else { 'own_s_f00' }
    Send-Display @('stereo off')
    [System.IO.File]::WriteAllText((Join-Path $game 'SVR_Ads.txt'), "eye left`n", $enc)
    Start-Sleep -Seconds 3
    $ownM1 = @(Compose 'own_m1' -World)[0]
    Start-Sleep -Milliseconds 400
    $ownM2 = @(Compose 'own_m2' -World)[0]
    [System.IO.File]::WriteAllText((Join-Path $game 'SVR_Ads.txt'), "eye center`n", $enc)
    Send-Display @('stereo on')
    $dOwn = Get-PairDiff $ownS $ownM1 'far'
    $dNull = Get-PairDiff $ownM1 $ownM2 'far'
    Check "stereo: each eye's image is its own" ($dOwn.eye0.mad -le [Math]::Max(1.0, 2.0 * $dNull.eye0.mad)) ("left eye, stereo vs mono-from-left: mad {0}; null pair {1}; shift {2} px" -f $dOwn.eye0.mad, $dNull.eye0.mad, (F ([Math]::Abs($dOwn.eye0.shift_px[0])) '0.00'))
    Start-Sleep -Seconds 2

    # Per eye, the world held still: compare an eye with itself two frames later.
    $tags = Compose 'stw' -World -Seq 6
    $d = Get-PairDiff $tags[1] $tags[3]
    $d2 = Get-PairDiff $tags[2] $tags[4]
    $sh = @($d.eye0.shift_px[0], $d.eye0.shift_px[1], $d.eye1.shift_px[0], $d.eye1.shift_px[1], $d2.eye0.shift_px[0], $d2.eye1.shift_px[0]) | ForEach-Object { [Math]::Abs($_) } | Measure-Object -Maximum
    Check 'stereo: each eye steady (still)' ($sh.Maximum -le 0.3) ("worst shift {0} px; mad {1}/{2}" -f (F $sh.Maximum '0.00'), $d.eye0.mad, $d.eye1.mad)

    # Head moving: each eye's world must not swim either.
    $tags = With-HeadMotion { Compose 'stws' -World -Seq 8 }
    $worst = 0.0
    for ($i = 2; $i -lt $tags.Count; $i++) {
        $d = Get-PairDiff $tags[$i - 2] $tags[$i]
        foreach ($e in 'eye0', 'eye1') {
            $worst = [Math]::Max($worst, [Math]::Max([Math]::Abs($d.$e.shift_px[0]), [Math]::Abs($d.$e.shift_px[1])))
        }
    }
    Check 'stereo: each eye steady (head moving)' ($worst -le 1.0) ("worst shift {0} px" -f (F $worst '0.00'))

    # Smooth artificial turn: the held eye is re-declared by the turn since it was drawn. Without that,
    # 2 rad/s at 60 fps is ~1.9 deg of false disparity on everything far.
    Pose -TurnX 0.8
    Start-Sleep -Milliseconds 700
    $tags = Compose 'stt' -Seq 4
    Pose
    $worstFar = 0.0
    foreach ($t in $tags) {
        $a = Analyse $t
        $worstFar = [Math]::Max($worstFar, [Math]::Abs($a.h_disparity_deg.median))
        $vd = $a.v_disparity_deg.abs_p95
    }
    Check 'stereo: turning keeps the far world fused' ($worstFar -le 1.5) ("worst far median {0} deg while turning" -f (F $worstFar '0.00'))
    if (-not $KeepStereo) { Send-Display @('stereo off') }
}

# ------------------------------------------------------------------------------------------------
# Frame rate, for the record (the mock paces at its configured rate, 90 Hz): mono and stereo.
function Measure-Fps([string]$Mode) {
    Send-Display @("stereo $Mode")
    Start-Sleep -Seconds 3
    [System.IO.File]::WriteAllText((Join-Path $game 'SVR_FrameCmd.txt'), "trace 150`n", $enc)
    Start-Sleep -Seconds 6
    $line = & python (Join-Path $PSScriptRoot 'frame_trace.py') (Join-Path $game 'SVR_FrameTrace.csv') | Select-String 'presents/s|present lag'
    return ($line -join ' | ')
}
if ($Only -eq 'all') {
    Write-Output ('fps mono:   ' + (Measure-Fps 'off'))
    Write-Output ('fps stereo: ' + (Measure-Fps 'on'))
    if (-not $KeepStereo) { Send-Display @('stereo off') }
}

Write-Output ''
$fail = @($script:results | Where-Object Result -eq 'FAIL').Count
Write-Output ("{0} checks, {1} failed. Pictures in {2}" -f $script:results.Count, $fail, $out)
