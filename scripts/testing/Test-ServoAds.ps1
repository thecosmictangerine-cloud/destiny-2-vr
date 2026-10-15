# The body servo in ADS: does the character still follow the gaze, and does the stuck guard stay quiet?
#
# The servo turns the character with mouse counts, at a counts-per-radian learned at the hip. In ADS
# the game scales look sensitivity down by its zoom, so the same counts turn the character less: the
# loop's gain drops by the zoom (1.5-1.8) and it settles more slowly. That is harmless as long as it
# still converges -- the view is the room anchor plus the head, and the shot leaves the barrel, so
# the character's lag costs only the legs and the walking direction (which vr_gamepad rotates by the
# lag anyway). What would NOT be harmless is the stuck guard (`servo=stuck`, 20 frames of a response
# under 25% of the request) reading ADS as an absent character and stopping for good.
#
# Measured from the ADS trace's `body` column (the servo's error every frame): a 30 deg head turn at
# the hip, then the same with ADS held (`force on`).
#   hip / ads   time from the head turn until the error stays under 2 deg (pass: under 1.5 s)
#   stuck       no `servo=stuck` in the whole run
param(
    [string]$Out = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'build\shots\servoads'),
    [double]$Turn = 30
)
$ErrorActionPreference = 'Stop'
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'lib\GameIO.ps1')
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'lib\VrCommands.ps1')
if (-not (Test-Path $Out)) { New-Item -ItemType Directory -Force -Path $Out | Out-Null }
$inv = [System.Globalization.CultureInfo]::InvariantCulture
$results = New-Object System.Collections.ArrayList
function Record([string]$step, [bool]$ok, [string]$detail) {
    [void]$results.Add([pscustomobject]@{ step = $step; verdict = $(if ($ok) { 'OK' } else { 'FAIL' }); detail = $detail })
    Write-Host ("{0,-6} {1,-5} {2}" -f $step, $(if ($ok) { 'OK' } else { 'FAIL' }), $detail)
}
function Get-T([string]$line) { if ($line -match ' t=(\d+) ') { return [long]$Matches[1] } return -1 }

[void](Focus-Game)
[void](Send-Ads @('mode auto', 'force off'))
$hand = @{ HandX = 0.0; HandY = 0.0; HandZ = -0.15 }
$p = @{} + $hand + @{ HeadYaw = 0 }; [void](Set-MockInput @p)
Start-Sleep -Seconds 3
$t0 = Get-T "$(Get-VrLog 'ev=vr\.' 1)"
[void](Send-Ads @('trace 60'))
Start-Sleep -Seconds 1.5
# Hip.
$p = @{} + $hand + @{ HeadYaw = $Turn }; [void](Set-MockInput @p); Start-Sleep -Seconds 3.5
$p = @{} + $hand + @{ HeadYaw = 0 }; [void](Set-MockInput @p); Start-Sleep -Seconds 3.5
# ADS.
[void](Send-Ads @('force on')); Start-Sleep -Seconds 2
$p = @{} + $hand + @{ HeadYaw = $Turn }; [void](Set-MockInput @p); Start-Sleep -Seconds 3.5
$p = @{} + $hand + @{ HeadYaw = 0 }; [void](Set-MockInput @p); Start-Sleep -Seconds 3.5
[void](Send-Ads @('force off', 'trace stop'))
Start-Sleep -Seconds 4
$csv = Join-Path $Out 'servo_trace.csv'
Copy-Item C:\Games\Sunrise\SVR_AdsTrace.csv $csv -Force

$analysis = & python -c @"
import csv, sys, math
rows = list(csv.DictReader(open(sys.argv[1])))
ms = [int(r['ms']) for r in rows]
err = [float(r['body']) for r in rows]
zoom = [float(r['zoom']) for r in rows]
steps = [i for i in range(1, len(err)) if abs(err[i] - err[i - 1]) > math.radians(15)]
for i in steps:
    j = i
    settle = None
    for k in range(i, len(err)):
        if abs(err[k]) > math.radians(2):
            j = k
        if ms[k] - ms[i] > 3000:
            break
    settle = ms[j] - ms[i] if j > i or abs(err[i]) > math.radians(2) else 0
    peak = max(abs(e) for e in err[i:i + 5])
    kind = 'ads' if zoom[i] > 1.05 else 'hip'
    print(f'STEP {kind} {settle} {math.degrees(peak):.1f} {math.degrees(abs(err[min(len(err) - 1, j + 3)])):.2f}')
"@ $csv
$analysis | ForEach-Object { Write-Host "  $_" }
foreach ($kind in @('hip', 'ads')) {
    $steps = @($analysis | Where-Object { $_ -match "^STEP $kind " })
    if ($steps.Count -eq 0) { Record $kind $false 'no head turn found in the trace'; continue }
    $worst = ($steps | ForEach-Object { [double](($_ -split ' ')[2]) } | Measure-Object -Maximum).Maximum
    Record $kind ($worst -lt 1500) ("{0} turns of {1} deg, slowest settles under 2 deg in {2} ms" -f $steps.Count, $Turn, $worst)
}
$stuck = @(Read-VrLog 40000 | Where-Object { (Get-T $_) -gt $t0 -and $_ -match 'servo=stuck' }).Count
Record 'stuck' ($stuck -eq 0) ("{0} stuck reports" -f $stuck)
$fails = @($results | Where-Object { $_.verdict -ne 'OK' }).Count
Write-Host ("{0}/{1} OK" -f ($results.Count - $fails), $results.Count)
