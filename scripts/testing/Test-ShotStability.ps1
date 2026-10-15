# The gun stays in the hand while the game animates it: shots and a reload with the controller STILL.
#
# The arm module's seats learn the weapon's static hold from the drawn hand. Chasing an animation
# instead (recoil, reload) turns the gun in the hand on its own, and the shot -- which leaves along
# the gun frame -- goes with it. Measured before the rest gate (ads::animation_at_rest): rounds 3.5-4
# deg low after the first, one 8 deg off, and the sight swung up to 72 deg off the gaze in a reload.
#
# What it checks, the controller held still throughout:
#   shots    every fired direction (`ev=vr.aim shot` new_fwd, before the game's own spread) within
#            0.3 deg of their mean: the shot follows the controller, not the animation.
#   sight    the detector's sight-to-gaze angle never moves more than 1 deg during the burst.
#   seat     the rotational seat and the translational seat end where they started (0.5 deg, 3 mm).
#   drift    the drawn GUN relative to the controller at rest after the burst vs before it (the ADS
#            trace's seenGun): under 3 mm and 0.5 deg. The animation may move it in between; that is
#            the game's recoil and reload, and it must come back. The floor is the weapon's idle sway,
#            0.02-0.33 deg over six runs; the defect this exists for was 4 deg.
param(
    [string]$Out = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'build\shots\shotstab'),
    [int]$Shots = 10,
    [int]$GapMs = 900
)
$ErrorActionPreference = 'Stop'
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'lib\GameIO.ps1')
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'lib\VrCommands.ps1')
if (-not (Test-Path $Out)) { New-Item -ItemType Directory -Force -Path $Out | Out-Null }
$inv = [System.Globalization.CultureInfo]::InvariantCulture
$results = New-Object System.Collections.ArrayList
function Record([string]$step, [bool]$ok, [string]$detail) {
    [void]$results.Add([pscustomobject]@{ step = $step; verdict = $(if ($ok) { 'OK' } else { 'FAIL' }); detail = $detail })
    Write-Host ("{0,-8} {1,-5} {2}" -f $step, $(if ($ok) { 'OK' } else { 'FAIL' }), $detail)
}
function Get-Vec([string]$text, [string]$name) {
    if ($text -notmatch "$name=([-\d.]+),([-\d.]+),([-\d.]+)") { return $null }
    return @([double]::Parse($Matches[1], $inv), [double]::Parse($Matches[2], $inv), [double]::Parse($Matches[3], $inv))
}
function Get-Num([string]$text, [string]$name) {
    if ($text -notmatch "$name=([-\d.]+)") { return [double]::NaN }
    return [double]::Parse($Matches[1], $inv)
}
function Get-T([string]$line) { if ($line -match ' t=(\d+) ') { return [long]$Matches[1] } return -1 }
# Fresh by timestamp, not by counting matches in the log's tail: that count stops growing once old
# matches scroll out of the window and hands back a stale line.
function Get-Seat {
    $since = (Read-VrLog 400 | ForEach-Object { Get-T $_ } | Measure-Object -Maximum).Maximum
    [void](Send-Arms @('dump'))
    $deadline = (Get-Date).AddSeconds(8)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 300
        $l = @(Read-VrLog 6000 | Where-Object { $_ -match 'ev=vr\.arms seat ' -and (Get-T $_) -gt $since })
        if ($l.Count -gt 0) { return $l[-1] }
    }
    return $null
}
# Normalised first: the log prints four decimals, and two copies of one rotation printed as
# (0.0058, 0.0027, -0.0107, 0.9999) have a norm of 0.99995 -- which reads as 1.1 deg apart.
function Quat-Angle($a, $b) {
    $na = [Math]::Sqrt($a[0] * $a[0] + $a[1] * $a[1] + $a[2] * $a[2] + $a[3] * $a[3])
    $nb = [Math]::Sqrt($b[0] * $b[0] + $b[1] * $b[1] + $b[2] * $b[2] + $b[3] * $b[3])
    $d = [Math]::Abs($a[0] * $b[0] + $a[1] * $b[1] + $a[2] * $b[2] + $a[3] * $b[3]) / ($na * $nb)
    return 2.0 * [Math]::Acos([Math]::Min(1.0, $d)) * 180.0 / [Math]::PI
}
function Get-Quat([string]$text, [string]$name) {
    if ($text -notmatch "$name=([-\d.]+),([-\d.]+),([-\d.]+),([-\d.]+)") { return $null }
    return @(1..4 | ForEach-Object { [double]::Parse($Matches[$_], $inv) })
}

[void](Focus-Game)
[void](Send-Ads @('mode auto', 'force off'))
# Held at the hip, the sight 5 cm off the eye: close enough that a swinging sight would enter ADS.
[void](Set-MockInput -HandX -0.18 -HandY 0.14 -HandZ -0.25 -HandYaw 0 -HandPitch 0)
Start-Sleep -Seconds 4
$seat0 = Get-Seat
$t0 = Get-T "$(Get-VrLog 'ev=vr\.' 1)"
[void](Send-Ads @('trace 30'))
Start-Sleep -Seconds 1.5
for ($i = 0; $i -lt $Shots; $i++) {
    [SunriseVR.Native]::mouse_event(0x0002, 0, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 50
    [SunriseVR.Native]::mouse_event(0x0004, 0, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds $GapMs
    if ($i -eq [int]($Shots / 2)) { Send-GameKey R; Start-Sleep -Seconds 3.5 }
}
Start-Sleep -Seconds 4
$seat1 = Get-Seat
# Let the trace run out and the log flush.
$deadline = (Get-Date).AddSeconds(40)
while ((Get-Date) -lt $deadline -and -not ("$(Get-VrLog 'ev=vr\.ads trace written' 1)" -match ' t=(\d+) ' -and [long]$Matches[1] -gt $t0)) {
    Start-Sleep -Seconds 1
}
Start-Sleep -Seconds 3
$lines = @(Read-VrLog 40000 | Where-Object { (Get-T $_) -gt $t0 })

# --- shots
# The leading comma keeps each vector one element: the pipeline would otherwise flatten them.
$dirs = @($lines | Where-Object { $_ -match 'ev=vr\.aim shot ' } | ForEach-Object { , (Get-Vec $_ 'new_fwd') } | Where-Object { $_ })
if ($dirs.Count -ge 3) {
    $m = @(0.0, 0.0, 0.0)
    foreach ($d in $dirs) { for ($k = 0; $k -lt 3; $k++) { $m[$k] += $d[$k] } }
    $n = [Math]::Sqrt($m[0] * $m[0] + $m[1] * $m[1] + $m[2] * $m[2])
    $worst = 0.0
    foreach ($d in $dirs) {
        $c = ($d[0] * $m[0] + $d[1] * $m[1] + $d[2] * $m[2]) / $n / [Math]::Sqrt($d[0] * $d[0] + $d[1] * $d[1] + $d[2] * $d[2])
        $worst = [Math]::Max($worst, [Math]::Acos([Math]::Min(1.0, $c)) * 180.0 / [Math]::PI)
    }
    Record 'shots' ($worst -lt 0.3) ("{0} rounds, worst {1:F2} deg from their mean" -f $dirs.Count, $worst)
} else {
    Record 'shots' $false ("only {0} rounds logged" -f $dirs.Count)
}

# --- sight
$gazes = @($lines | Where-Object { $_ -match 'ev=vr\.ads state=' } | ForEach-Object { Get-Num $_ 'gaze_deg' })
$enters = @($lines | Where-Object { $_ -match 'ev=vr\.ads enter ' }).Count
if ($gazes.Count -ge 3) {
    $span = ($gazes | Measure-Object -Maximum -Minimum)
    Record 'sight' (($span.Maximum - $span.Minimum) -lt 1.0 -and $enters -eq 0) ("gaze {0:F2}..{1:F2} deg over {2} reports, {3} ADS entries" -f $span.Minimum, $span.Maximum, $gazes.Count, $enters)
} else {
    Record 'sight' $false ("only {0} state reports" -f $gazes.Count)
}

# --- seat
$q0 = Get-Quat "$seat0" ' rot'; $q1 = Get-Quat "$seat1" ' rot'
$c0 = Get-Vec "$seat0" 'correction'; $c1 = Get-Vec "$seat1" 'correction'
if ($q0 -and $q1 -and $c0 -and $c1) {
    $dq = Quat-Angle $q0 $q1
    $dc = [Math]::Sqrt([Math]::Pow($c1[0] - $c0[0], 2) + [Math]::Pow($c1[1] - $c0[1], 2) + [Math]::Pow($c1[2] - $c0[2], 2)) * 1000
    Record 'seat' ($dq -lt 0.5 -and $dc -lt 3.0) ("rotational seat moved {0:F2} deg, translational {1:F1} mm" -f $dq, $dc)
} else {
    Record 'seat' $false "no seat report: $seat0 / $seat1"
}

# --- drift: the drawn gun vs the controller, at rest before and after
$csv = Join-Path $Out 'burst_trace.csv'
Copy-Item C:\Games\Sunrise\SVR_AdsTrace.csv $csv -Force
$drift = & python -c @"
import csv, math, sys
rows = [r for r in csv.DictReader(open(sys.argv[1])) if r['gun'] == '1']
def pose(r): return [float(r[k]) for k in ('vx','vy','vz')], [float(r[k]) for k in ('vqx','vqy','vqz','vqw')]
head, tail = rows[:30], rows[-30:]
def mean(rs):
    t = [sum(pose(r)[0][i] for r in rs) / len(rs) for i in range(3)]
    q0 = pose(rs[0])[1]
    q = [0.0] * 4
    for r in rs:
        qi = pose(r)[1]
        s = 1 if sum(a * b for a, b in zip(qi, q0)) >= 0 else -1
        q = [a + s * b for a, b in zip(q, qi)]
    n = math.sqrt(sum(a * a for a in q))
    return t, [a / n for a in q]
(ta, qa), (tb, qb) = mean(head), mean(tail)
dt = math.dist(ta, tb) * 1000
dq = 2 * math.degrees(math.acos(min(1.0, abs(sum(a * b for a, b in zip(qa, qb))))))
peak = max(math.dist(pose(r)[0], ta) for r in rows) * 1000
print(f'{dt:.2f} {dq:.3f} {peak:.1f} {len(rows)}')
"@ $csv
$parts = "$drift".Trim() -split ' '
if ($parts.Count -eq 4) {
    $dt = [double]::Parse($parts[0], $inv); $dq = [double]::Parse($parts[1], $inv)
    Record 'drift' ($dt -lt 3.0 -and $dq -lt 0.5) ("gun vs controller at rest, after vs before: {0} mm, {1} deg (the animation took it {2} mm away in between; {3} rows)" -f $parts[0], $parts[1], $parts[2], $parts[3])
} else {
    Record 'drift' $false "trace unreadable: $drift"
}

Write-Host ''
$fails = @($results | Where-Object { $_.verdict -ne 'OK' }).Count
Write-Host ("{0}/{1} OK" -f ($results.Count - $fails), $results.Count)
