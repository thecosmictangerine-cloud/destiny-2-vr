# The arm IK matrix on the mock, in a running session (in a first-person area, F9 on).
#
# Each step sets both controllers, waits for the pose to settle, and takes a screenshot with the
# verification overlay on: controller frames (RGB = forward, left, up), the solved arm (yellow
# shoulder -> elbow -> wrist, orange when the shoulder had to slide), the hand target frame. The
# log line after each step carries the solver's own numbers (stretch, twist, wrist error).
#
# What to look for in the PNGs:
#   - each hand sits on its controller frame; the right one also on the gun's grip
#   - the yellow chain runs INSIDE the sleeve all the way (a line outside it is a bug)
#   - elbows bend down and out, never up or through the body
#   - the wrist turns with the controller's roll and the forearm twists with it, without candy-wrapping
param(
    [string]$Out = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'build\shots\arms'),
    [double]$Settle = 1.2,
    [switch]$NoOverlay,
    [string[]]$Only
)
$ErrorActionPreference = 'Stop'
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'lib\GameIO.ps1')
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'lib\VrCommands.ps1')
if (-not (Test-Path $Out)) { New-Item -ItemType Directory -Force -Path $Out | Out-Null }

# Offsets are added to the mock's rest positions: right hand (0.2, 1.4, -0.3), left (-0.2, 1.4, -0.3),
# OpenXR stage metres (+X right, +Y up, -Z forward); the eye is at 1.6.
$steps = [ordered]@{
    'a_rest'        = @{ HandX = 0.0;   HandY = 0.10; HandZ = -0.15; LHandX = 0.05;  LHandY = 0.08;  LHandZ = -0.12 }
    'b_left_up'     = @{ HandX = 0.0;   HandY = 0.10; HandZ = -0.15; LHandX = -0.10; LHandY = 0.35;  LHandZ = -0.15 }
    'c_left_reach'  = @{ HandX = 0.0;   HandY = 0.10; HandZ = -0.15; LHandX = 0.10;  LHandY = 0.15;  LHandZ = -0.45 }
    'd_left_roll'   = @{ HandX = 0.0;   HandY = 0.10; HandZ = -0.15; LHandX = 0.05;  LHandY = 0.08;  LHandZ = -0.12; LHandRoll = 80 }
    'e_left_yaw'    = @{ HandX = 0.0;   HandY = 0.10; HandZ = -0.15; LHandX = 0.05;  LHandY = 0.08;  LHandZ = -0.12; LHandYaw = 45 }
    'f_right_cross' = @{ HandX = -0.25; HandY = 0.12; HandZ = -0.15; LHandX = 0.0;   LHandY = 0.0;   LHandZ = -0.05 }
    'g_right_roll'  = @{ HandX = 0.0;   HandY = 0.10; HandZ = -0.15; HandRoll = 60; LHandX = 0.05; LHandY = 0.08; LHandZ = -0.12 }
    'h_left_pitch'  = @{ HandX = 0.0;   HandY = 0.10; HandZ = -0.15; LHandX = 0.05;  LHandY = 0.08;  LHandZ = -0.12; LHandPitch = -50 }
    'i_both_close'  = @{ HandX = -0.05; HandY = 0.05; HandZ = 0.10;  LHandX = 0.08;  LHandY = 0.05;  LHandZ = 0.10 }
    'j_two_hand'    = @{ HandX = -0.05; HandY = 0.10; HandZ = -0.15; LHandX = 0.18;  LHandY = 0.08;  LHandZ = -0.30 }
}

[void](Focus-Game)
if (-not $NoOverlay) { [void](Send-Arms @('overlay on')) }
foreach ($name in $steps.Keys) {
    if ($Only -and ($Only -notcontains $name)) { continue }
    $p = $steps[$name]
    [void](Set-MockInput @p)
    Start-Sleep -Seconds $Settle
    $shot = Join-Path $Out "$name.png"
    Save-Shot $shot | Out-Null
    [void](Send-Arms @('dump'))
    Start-Sleep -Milliseconds 700
    $state = Get-VrLog 'ev=vr\.arms state' 1
    Write-Output ("{0,-14} {1}" -f $name, ($state -replace '^.*ev=vr\.arms state ', ''))
}
$rest = $steps['a_rest']
[void](Set-MockInput @rest)
