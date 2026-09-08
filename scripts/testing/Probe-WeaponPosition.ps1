# Sweeps one weapon configuration in about twenty seconds and answers a single question: does the
# gun's POSITION respond to anything?
#
# The orientation half of the pose is already known to reach the weapon (getter D5D832), and the
# position half is already known not to. This script exists to find a path that does carry it, so
# it uses a deliberately absurd offset -- a metre and a half sideways -- because a real 6DOF
# response would throw the gun clean out of the frame, and nothing subtler than that is worth
# looking at while hunting for the path.
#
# Two shots per configuration: hand at rest, hand a long way right. Anything that moves the gun
# between them is the lever being looked for.
param(
    [Parameter(Mandatory)][string]$Tag,
    [string[]]$Commands,
    [double]$Offset = 1.5,
    [string]$GameDir = 'C:\Games\Sunrise',
    [int]$SettleSeconds = 4
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
    param([string[]]$Lines)
    $before = ((Read-LogLines) | Select-String 'ev=vr\.weapon rule|ev=vr\.weapon block|ev=vr\.weapon hand_offset').Count
    [IO.File]::WriteAllText((Join-Path $GameDir 'SVR_Weapon.txt'), (($Lines -join "`n") + "`n"),
        (New-Object System.Text.UTF8Encoding($false)))
    Start-Sleep -Seconds 3
    (Read-LogLines) | Select-String 'ev=vr\.weapon rule|ev=vr\.weapon block|ev=vr\.weapon hand_offset' |
        Select-Object -Skip $before | ForEach-Object { '    ' + ($_.Line -replace '.*client level=warn ', '') }
}

$zero = @{ HeadYaw = 0; HeadPitch = 0; HeadX = 0; HeadY = 0; HeadZ = 0
    HandYaw = 0; HandPitch = 0; HandX = 0; HandY = 0; HandZ = 0
    LHandYaw = 0; LHandPitch = 0; LHandX = 0; LHandY = 0; LHandZ = 0 }

function Shot {
    param([string]$Name, [hashtable]$Changes)
    $mock = $zero.Clone()
    foreach ($k in $Changes.Keys) { $mock[$k] = $Changes[$k] }
    Set-MockInput @mock | Out-Null
    Start-Sleep -Seconds $SettleSeconds
    [void](Focus-Game)
    Save-Shot (Join-Path $shots ("{0}_{1}.png" -f $Tag, $Name)) | Out-Null
}

if (-not (Get-GameProcess)) { throw 'game is not running' }
[void](Focus-Game)
Write-Output "== $Tag : $($Commands -join ' | ') =="
Send-Weapon (@('install') + $Commands)
Shot 'a_rest' @{}
Shot 'b_right' @{ HandX = $Offset }
Shot 'c_near'  @{ HandZ = $Offset }
Set-MockInput @zero | Out-Null
python (Join-Path $PSScriptRoot 'weapon_metric.py') $shots $Tag
