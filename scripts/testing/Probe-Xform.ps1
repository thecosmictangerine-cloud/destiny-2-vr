# Works out what the eight floats of the weapon's transform buffer mean, by poking them.
#
# The buffer is what `destiny2.exe+0xD5D7F0` hands its caller after turning the camera basis into
# whatever form the weapon placement wants. Guessing its layout from the disassembly of the
# conversion routine would take longer than measuring it, and measuring it is cheap: force one
# lane at a time to a value far outside its normal range and see what the gun does.
#
# Reading the result:
#   gun vanishes or flies off      that lane is positional or a scale
#   gun rotates in place           that lane is part of the orientation
#   nothing happens                padding, or a value the caller ignores
#
# Each lane is restored before the next, so the sweep cannot accumulate state.
param(
    [string]$Tag = 'x1',
    [double]$Value = 40.0,
    [string]$GameDir = 'C:\Games\Sunrise',
    [int]$SettleSeconds = 3
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
function Send-Weapon {
    param([string[]]$Lines, [int]$WaitMs = 2200)
    [IO.File]::WriteAllText((Join-Path $GameDir 'SVR_Weapon.txt'), (($Lines -join "`n") + "`n"),
        (New-Object System.Text.UTF8Encoding($false)))
    Start-Sleep -Milliseconds $WaitMs
}
function Shot { param([string]$Name)
    Start-Sleep -Seconds $SettleSeconds
    [void](Focus-Game)
    Save-Shot (Join-Path $shots ("{0}_{1}.png" -f $Tag, $Name)) | Out-Null
}

$zero = @{ HeadYaw = 0; HeadPitch = 0; HeadX = 0; HeadY = 0; HeadZ = 0
    HandYaw = 0; HandPitch = 0; HandX = 0; HandY = 0; HandZ = 0
    LHandYaw = 0; LHandPitch = 0; LHandX = 0; LHandY = 0; LHandZ = 0 }

if (-not (Get-GameProcess)) { throw 'game is not running' }
[void](Focus-Game)
Set-MockInput @zero | Out-Null

Write-Output '== installing and dumping the buffer at rest =='
$before = ((Read-LogLines) | Select-String 'ev=vr\.weapon xform caller').Count
Send-Weapon @('install', 'block on', 'getter none x', 'xform clear', 'xform dump on') 4000
Start-Sleep -Seconds 4
(Read-LogLines) | Select-String 'ev=vr\.weapon xform caller' | Select-Object -Skip $before -First 3 |
    ForEach-Object { '  ' + ($_.Line -replace '.*client level=warn ', '') }
Shot '00_rest'

Write-Output ''
Write-Output "== forcing each lane to $Value in turn =="
for ($lane = 0; $lane -lt 8; $lane++) {
    Send-Weapon @('xform clear', "xform force $lane $Value")
    Shot ("{0:d2}_lane{1}" -f ($lane + 1), $lane)
    Write-Output ("  lane $lane forced")
}
Send-Weapon @('xform clear')
Shot '09_restored'

python (Join-Path $PSScriptRoot 'weapon_metric.py') $shots $Tag
