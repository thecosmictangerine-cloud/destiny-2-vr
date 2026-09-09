# Finds the pivot by LOOKING, with a large rotation, and blends the two frames so the answer is
# obvious rather than inferred.
#
# Every numeric route was tried first and each hit a wall worth recording, because they are the
# reasons this file exists:
#
#   least squares on screen pixels   the pivot's forward axis turns out to point along the camera's
#                                    own view direction, so moving it changes the weapon's DEPTH --
#                                    0.15 m forward moved the gun 88 px where the same 0.15 m
#                                    sideways moved it 253. The Jacobian was near singular in
#                                    exactly the axis being measured.
#   a one-dimensional scan of yaw    a yawed gun FORESHORTENS, and cross-correlation answers with
#                                    the offset that best fits a changed shape. That bias is worth
#                                    about 100 px with no translation behind it, it is constant in
#                                    the pivot, and between 0 and -|d| the true signal is linear --
#                                    so slope and bias cannot be separated.
#   scanning past the vertex         would separate them, but pulls the weapon towards the eye
#                                    until it leaves the template box, and a box holding only
#                                    scenery reports a confident 0 px shift. Three points of one
#                                    scan were false zeros at peaks up to 0.97.
#
# What survives all of that is the definition itself: with the pivot right, the weapon's model
# origin sits AT the palm, so rotating the wrist turns the gun about a fixed point. Blend the two
# rotations into one image and a fixed point is something you can see in a second, where 100 px of
# correlation bias is not.
#
# The measurement noise floor for this scene was established first, and it is essentially nil: the
# weapon moved 0 px over 20 seconds of standing still, and 0 px across firing a shot (the ammo
# counter went 13 -> 12, so the shot really happened). So the idle animation is not moving this
# weapon, and anything seen here is the pivot.
param(
    [string]$GameDir = 'C:\Games\Sunrise',
    [string]$Tag = 'pv',
    [double[]]$Forward = @(0, -0.17, -0.33, -0.50),
    # Large on purpose. A small rotation makes a wrong pivot look almost right; 70 degrees of hand
    # pitch swings the barrel from level to nearly vertical and makes the centre of rotation plain.
    [double]$Pitch = 40,
    # Where the hand is held, as an offset on the mock's default (0.2 right, 0.2 below, 0.3 forward
    # of the eye). The default is closer than anyone holds a controller, and it matters here rather
    # than being cosmetic: with the pivot correct the weapon's origin sits ON the palm, so a palm
    # 0.2 m below and 0.3 m in front of the eye puts the weapon below the field of view entirely.
    # It looked like the pivot was throwing the gun out of frame; it was the fixture.
    [double]$HoldX = 0.05,
    [double]$HoldY = -0.10,
    [double]$HoldZ = -0.15,
    [int]$SettleSeconds = 3
)
$ErrorActionPreference = 'Stop'
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'lib\GameIO.ps1')
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$shots = Join-Path $repo 'build\shots'
$inv = [System.Globalization.CultureInfo]::InvariantCulture

function Send-Weapon {
    param([string[]]$Lines)
    [IO.File]::WriteAllText((Join-Path $GameDir 'SVR_Weapon.txt'), (($Lines -join "`n") + "`n"),
        (New-Object System.Text.UTF8Encoding($false)))
    Start-Sleep -Seconds 3
}
function Fmt([double]$v) { return $v.ToString($inv) }

$zero = @{ HeadYaw = 0; HeadPitch = 0; HeadX = 0; HeadY = 0; HeadZ = 0
    HandYaw = 0; HandPitch = 0; HandRoll = 0; HandX = $HoldX; HandY = $HoldY; HandZ = $HoldZ
    LHandYaw = 0; LHandPitch = 0; LHandRoll = 0; LHandX = 0; LHandY = 0; LHandZ = 0 }

function Capture {
    param([string]$Name, [hashtable]$Changes = @{})
    $mock = $zero.Clone()
    foreach ($k in $Changes.Keys) { $mock[$k] = $Changes[$k] }
    Set-MockInput @mock | Out-Null
    Start-Sleep -Seconds $SettleSeconds
    [void](Assert-GameFocus)
    $file = "{0}_{1}.png" -f $Tag, $Name
    Save-Shot (Join-Path $shots $file) | Out-Null
    return $file
}

if (-not (Get-GameProcess)) { throw 'game is not running; run Run-Patrol.ps1 first' }
[void](Assert-GameFocus)
# Out of the idle pose before anything is measured -- see Clear-WeaponIdle.
[void](Clear-WeaponIdle)
Send-Weapon @('install', 'block on', 'getter D5D832 hand', 'xform clear', 'xform lanes 4 5 6',
    'xform delta on', 'anchor palm')

$pairs = New-Object System.Collections.Generic.List[object]
foreach ($p in $Forward) {
    $slug = 'f{0:d3}' -f [int]([Math]::Abs($p) * 100)
    Send-Weapon @(('pivot {0} 0 0' -f (Fmt $p)))
    $a = Capture ($slug + '_level')
    $b = Capture ($slug + '_pitched') @{ HandPitch = $Pitch }
    $pairs.Add([pscustomobject]@{ Pivot = ([double]$p).ToString('F3', $inv); Level = $a; Pitched = $b })
    Write-Output ("  pivot fwd {0,6:f3}  ->  {1}  |  {2}" -f $p, $a, $b)
}
Send-Weapon @('pivot 0 0 0')
Set-MockInput @zero | Out-Null

$csv = Join-Path $shots ("{0}_pairs.csv" -f $Tag)
$pairs | Export-Csv -NoTypeInformation -Path $csv
Write-Output ''
Write-Output '== building the blend sheet: look for the pivot value where the GRIP stays put =='
python (Join-Path $PSScriptRoot 'pivot_sheet.py') $shots $csv
Write-Output ("sheet -> {0}\{1}_sheet.png" -f $shots, $Tag)
