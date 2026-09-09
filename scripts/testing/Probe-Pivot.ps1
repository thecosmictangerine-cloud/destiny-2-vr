# Reconnaissance before the pivot calibration: where does the weapon actually SIT now?
#
# This exists because of a specific way the calibration could report a false pass. The new
# placement is absolute -- the gun goes where the controller really is, not to the engine's tidy
# viewmodel spot -- so its rest position moves, by roughly |palm - head|. `weapon_shift.py` takes
# its template from a box that was authored around the OLD rest position. If the gun has left that
# box, the template is background, the background does not move under a pure hand rotation, and the
# test would measure a beautiful zero for the wrong reason.
#
# So: capture a few states, print where the module thinks everything is, and let the pictures be
# looked at before anything is believed. No verdicts here on purpose.
param(
    [string]$GameDir = 'C:\Games\Sunrise',
    [string]$Tag = 'pp',
    [ValidateSet('palm', 'aim')][string]$Anchor = 'palm',
    [double[]]$Pivot = @(0, 0, 0),
    [double]$Angle = 15,
    [int]$SettleSeconds = 4
)
$ErrorActionPreference = 'Stop'
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'lib\GameIO.ps1')
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$shots = Join-Path $repo 'build\shots'
if (-not (Test-Path $shots)) { New-Item -ItemType Directory -Force -Path $shots | Out-Null }
$log = Join-Path $GameDir 'bin\x64\Sunrise\logs\sunrise.log'
$inv = [System.Globalization.CultureInfo]::InvariantCulture

function Read-LogLines {
    $stream = [IO.File]::Open($log, 'Open', 'Read', 'ReadWrite')
    try {
        $reader = New-Object IO.StreamReader($stream)
        try { return $reader.ReadToEnd() -split "`r?`n" } finally { $reader.Dispose() }
    } finally { $stream.Dispose() }
}
function Send-Weapon {
    param([string[]]$Lines)
    $before = ((Read-LogLines) | Select-String 'ev=vr\.weapon').Count
    [IO.File]::WriteAllText((Join-Path $GameDir 'SVR_Weapon.txt'), (($Lines -join "`n") + "`n"),
        (New-Object System.Text.UTF8Encoding($false)))
    Start-Sleep -Seconds 3
    (Read-LogLines) | Select-String 'ev=vr\.weapon' | Select-Object -Skip $before |
        ForEach-Object { '    ' + ($_.Line -replace '.*client level=warn ', '') }
}
function Fmt([double]$v) { return $v.ToString($inv) }

$zero = @{ HeadYaw = 0; HeadPitch = 0; HeadX = 0; HeadY = 0; HeadZ = 0
    HandYaw = 0; HandPitch = 0; HandRoll = 0; HandX = 0; HandY = 0; HandZ = 0
    LHandYaw = 0; LHandPitch = 0; LHandRoll = 0; LHandX = 0; LHandY = 0; LHandZ = 0 }
$index = 0
function Capture {
    param([string]$Name, [hashtable]$Changes = @{})
    $script:index++
    $mock = $zero.Clone()
    foreach ($k in $Changes.Keys) { $mock[$k] = $Changes[$k] }
    Set-MockInput @mock | Out-Null
    Start-Sleep -Seconds $SettleSeconds
    [void](Assert-GameFocus)
    $file = "{0}_{1:d2}_{2}.png" -f $Tag, $script:index, $Name
    Save-Shot (Join-Path $shots $file) | Out-Null
    $hand = (Read-LogLines) | Select-String 'ev=vr\.xr hand' | Select-Object -Last 1
    # Write-Host, not Write-Output: this function returns the file name, and anything written to the
    # output stream inside it would be returned alongside -- and then discarded with it by the
    # [void] at the call sites, which is exactly what swallowed these lines the first time.
    Write-Host ("[{0:d2}] {1}" -f $script:index, $file)
    if ($hand) { Write-Host ('     ' + ($hand.Line -replace '.*ev=vr\.xr hand ', '')) }
    return $file
}

if (-not (Get-GameProcess)) { throw 'game is not running; run Run-Patrol.ps1 first' }
[void](Assert-GameFocus)
if (-not ((Read-LogLines) | Select-String 'ev=vr\.xr init result=ok')) {
    throw 'no OpenXR session in the log -- press F9 in world first'
}

Write-Output '== what came up: the grip spaces and the weapon module =='
(Read-LogLines) | Select-String 'ev=vr\.xr actions|ev=vr\.weapon install|ev=vr\.weapon defaults' |
    Select-Object -Last 6 | ForEach-Object { '    ' + ($_.Line -replace '.*client level=warn ', '') }

Write-Output ''
Write-Output "== configuring: anchor=$Anchor pivot=$($Pivot -join ',') =="
Send-Weapon @('install', 'block on', 'getter D5D832 hand', 'xform clear', 'xform lanes 4 5 6',
    'xform delta on', "anchor $Anchor",
    ('pivot {0} {1} {2}' -f (Fmt $Pivot[0]), (Fmt $Pivot[1]), (Fmt $Pivot[2])), 'xform dump on')

Write-Output ''
Write-Output '== captures: is the gun on screen, and is it inside the template box? =='
[void](Capture 'rest')
[void](Capture 'roll_pos' @{ HandRoll = $Angle })
[void](Capture 'roll_neg' @{ HandRoll = -$Angle })
[void](Capture 'hand_right' @{ HandX = 0.15 })
[void](Capture 'rest_again')

Send-Weapon @('xform dump off')
Set-MockInput @zero | Out-Null

Write-Output ''
Write-Output '== what the module wrote into the lanes (in= engine, out= after our offset) =='
(Read-LogLines) | Select-String 'ev=vr\.weapon xform caller' | Select-Object -Last 8 |
    ForEach-Object { '    ' + ($_.Line -replace '.*client level=warn ', '') }
Write-Output ''
Write-Output '== callers of each detoured path =='
Send-Weapon @('report')
Write-Output ''
Write-Output ("client rect: {0}" -f (Export-ClientRect))
Write-Output ("shots -> {0}\{1}_*.png" -f $shots, $Tag)
