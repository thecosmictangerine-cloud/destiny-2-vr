# The F3 milestone test: is the weapon really 6DOF on the controller, and really decoupled from
# the head, in BOTH directions?
#
# Two halves, because either one alone can pass while the thing is still broken:
#
#   hand moves, head still  -- the weapon must follow the controller through all six degrees
#   head moves, hand still  -- the weapon must stay NAILED TO THE WORLD. Lean in and it grows and
#                              slides to the opposite side of the screen; lean back and it recedes.
#                              If it travels with the head, the engine is not honouring the
#                              position part of the pose it is handed, which is the known top risk
#                              of this phase.
#
# Every step writes a screenshot and pulls the frame's own numbers out of the log beside it, so a
# failure can be told apart from a mis-set input without re-running anything.
#
# Wants the game already in a first-person destination with F9 on: run Run-Patrol.ps1 first.
param(
    [string]$GameDir = 'C:\Games\Sunrise',
    [string]$Tag = 'w6',
    [ValidateSet('hand', 'body', 'head')][string]$Source = 'hand',
    [string]$Getter = 'D5D832',
    # `block pos|orient on|off`. The default is the configuration under test: head into the block,
    # hand to the weapon's own getter caller.
    [string[]]$Block = @('block on'),
    # The weapon's position, which the pose it is handed does not carry. The transform buffer's
    # world-space offset lanes do, so they get the controller's travel relative to its rest pose.
    [string[]]$Xform = @('xform clear', 'xform lanes 4 5 6', 'xform delta on'),
    [double[]]$HandOffset = $null,
    # Extra weapon commands applied after the ones above. This is how a second rule reaches the
    # same path, e.g. -Extra 'getter B363FA head' to hand the render view the head while the
    # weapon's own caller keeps the hand.
    [string[]]$Extra = @(),
    # Only the steps that decide whether the position part of the pose is honoured at all.
    [switch]$Quick,
    [int]$SettleSeconds = 4
)
$ErrorActionPreference = 'Stop'
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'lib\GameIO.ps1')
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$shots = Join-Path $repo 'build\shots'
if (-not (Test-Path $shots)) { New-Item -ItemType Directory -Force -Path $shots | Out-Null }
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
    $before = ((Read-LogLines) | Select-String 'ev=vr\.weapon').Count
    [IO.File]::WriteAllText((Join-Path $GameDir 'SVR_Weapon.txt'), (($Lines -join "`n") + "`n"),
        (New-Object System.Text.UTF8Encoding($false)))
    Start-Sleep -Seconds 3
    (Read-LogLines) | Select-String 'ev=vr\.weapon' | Select-Object -Skip $before |
        ForEach-Object { '    ' + ($_.Line -replace '.*client level=warn ', '') }
}

# Every field explicit on every call: the mock keeps the previous value for anything omitted, so a
# partial write would carry the last step's state into this one and look like a bug in the module.
$zero = @{ HeadYaw = 0; HeadPitch = 0; HeadX = 0; HeadY = 0; HeadZ = 0
    HandYaw = 0; HandPitch = 0; HandX = 0; HandY = 0; HandZ = 0
    LHandYaw = 0; LHandPitch = 0; LHandX = 0; LHandY = 0; LHandZ = 0 }

$index = 0
$rows = New-Object System.Collections.Generic.List[object]

# Applies one input state, waits, screenshots, and records the frame's own hand and camera numbers.
function Step {
    param([string]$Name, [hashtable]$Changes, [string]$Expect)
    $script:index++
    $mock = $zero.Clone()
    foreach ($k in $Changes.Keys) { $mock[$k] = $Changes[$k] }
    $handBefore = ((Read-LogLines) | Select-String 'ev=vr\.xr hand').Count
    Set-MockInput @mock | Out-Null
    Start-Sleep -Seconds $SettleSeconds
    [void](Focus-Game)
    $file = Join-Path $shots ("{0}_{1:d2}_{2}.png" -f $Tag, $script:index, $Name)
    Save-Shot $file | Out-Null
    $lines = (Read-LogLines) | Select-String 'ev=vr\.xr hand'
    $hand = if ($lines.Count -gt $handBefore) { $lines[-1].Line } else { '(stale)' }
    $probe = (Read-LogLines) | Select-String 'ev=vr\.probe pose' | Select-Object -Last 1
    $rows.Add([pscustomobject]@{
            Step = $script:index; Name = $Name; Expect = $Expect
            Shot = Split-Path -Leaf $file
            Hand = ($hand -replace '.*ev=vr\.xr hand ', '')
            Probe = if ($probe) { ($probe.Line -replace '.*ev=vr\.probe pose ', '') } else { '' }
        })
    Write-Output ("[{0:d2}] {1,-22} {2}" -f $script:index, $Name, $Expect)
    Write-Output ("     " + ($hand -replace '.*ev=vr\.xr hand ', ''))
}

if (-not (Get-GameProcess)) { throw 'game is not running; run Run-Patrol.ps1 first' }
[void](Focus-Game)
if (-not ((Read-LogLines) | Select-String 'ev=vr\.xr init result=ok')) {
    throw 'no OpenXR session in the log -- press F9 in world first'
}

Write-Output "== configuring: $Source on getter $Getter, $($Block -join ' / ') =="
$inv = [System.Globalization.CultureInfo]::InvariantCulture
$commands = @('install') + $Block + @("getter $Getter $Source") + $Xform
if ($HandOffset) {
    # Invariant formatting is not optional: this machine's locale writes 0.6 as "0,6", and the
    # module's sscanf stops at the comma and reads zero. That cost an hour once already.
    $commands += ('hand_offset {0} {1} {2}' -f ([double]$HandOffset[0]).ToString($inv),
        ([double]$HandOffset[1]).ToString($inv), ([double]$HandOffset[2]).ToString($inv))
}
$commands += $Extra
Send-Weapon $commands
Send-Weapon @('report')

if ($Quick) {
    Write-Output ''
    Write-Output '== QUICK: does the engine honour the position at all? =='
    Step 'q_rest'        @{}                'reference frame'
    # Same input as the step before: the difference between these two is the scene's own noise
    # floor (grass, blinking lights, the HUD), which every other row has to be judged against.
    Step 'q_rest_again'  @{}                'NOISE FLOOR -- nothing changed'
    Step 'q_hand_right'  @{ HandX = 0.35 }  'gun moves right on screen'
    Step 'q_hand_back'   @{ HandZ = 0.35 }  'gun comes closer, gets bigger'
    Step 'q_head_lean_in' @{ HeadZ = -0.4 } 'GUN GROWS and stays put in the world'
    Step 'q_head_lean_back' @{ HeadZ = 0.4 } 'GUN RECEDES and stays put in the world'
    Step 'q_head_right'  @{ HeadX = 0.3 }   'gun slides LEFT on screen'
    Set-MockInput @zero | Out-Null
    $csv = Join-Path $shots ("{0}_summary.csv" -f $Tag)
    $rows | Export-Csv -NoTypeInformation -Path $csv
    Write-Output ''
    Write-Output "shots  -> $shots\$Tag`_*.png"
    Send-Weapon @('report')
    exit 0
}

Write-Output ''
Write-Output '== HALF ONE: the hand moves, the head is still =='
Step 'rest'          @{}                        'reference frame for everything below'
Step 'hand_yaw_pos'  @{ HandYaw = 30 }          'gun rotates left, world does not move'
Step 'hand_yaw_neg'  @{ HandYaw = -30 }         'gun rotates right, world does not move'
Step 'hand_pitch_up' @{ HandPitch = 25 }        'gun points up'
Step 'hand_pitch_dn' @{ HandPitch = -25 }       'gun points down'
Step 'hand_right'    @{ HandX = 0.3 }           'gun moves right on screen'
Step 'hand_left'     @{ HandX = -0.3 }          'gun moves left on screen'
Step 'hand_up'       @{ HandY = 0.3 }           'gun moves up on screen'
Step 'hand_down'     @{ HandY = -0.3 }          'gun moves down on screen'
Step 'hand_fwd'      @{ HandZ = -0.35 }         'gun moves away, gets smaller'
Step 'hand_back'     @{ HandZ = 0.35 }          'gun comes closer, gets bigger'

Write-Output ''
Write-Output '== HALF TWO: the head moves, the hand is still -- the decoupling proof =='
Step 'head_rest'     @{}                        'back to the reference frame'
Step 'head_lean_in'  @{ HeadZ = -0.4 }          'GUN GROWS and stays put in the world'
Step 'head_lean_back' @{ HeadZ = 0.4 }          'GUN RECEDES and stays put in the world'
Step 'head_lean_right' @{ HeadX = 0.3 }         'gun slides LEFT on screen (opposite the head)'
Step 'head_lean_left' @{ HeadX = -0.3 }         'gun slides RIGHT on screen'
Step 'head_rise'     @{ HeadY = 0.25 }          'gun slides DOWN on screen'
Step 'head_crouch'   @{ HeadY = -0.25 }         'gun slides UP on screen'
Step 'head_yaw_pos'  @{ HeadYaw = 30 }          'world turns left, gun stays in the world'
Step 'head_yaw_neg'  @{ HeadYaw = -30 }         'world turns right, gun stays in the world'
Step 'head_pitch_up' @{ HeadPitch = 20 }        'world tips, gun stays in the world'
Step 'head_pitch_dn' @{ HeadPitch = -20 }       'world tips, gun stays in the world'

Write-Output ''
Write-Output '== BOTH AT ONCE: bringing the gun to the face =='
Step 'inspect'       @{ HeadZ = -0.25; HandZ = 0.2; HandY = 0.15 } 'gun close and centred, held up'

Set-MockInput @zero | Out-Null
$csv = Join-Path $shots ("{0}_summary.csv" -f $Tag)
$rows | Export-Csv -NoTypeInformation -Path $csv
Write-Output ''
Write-Output "shots  -> $shots\$Tag`_*.png"
Write-Output "summary-> $csv"
Write-Output ''
Write-Output '== measured weapon travel, in pixels, against the rest frame =='
$all = Get-ChildItem (Join-Path $shots ("{0}_*.png" -f $Tag)) | Sort-Object Name | ForEach-Object { $_.Name }
python (Join-Path $PSScriptRoot 'weapon_shift.py') $shots $all[0] @($all[1..($all.Count - 1)])
Write-Output ''
Write-Output '== weapon caller counters =='
Send-Weapon @('report')
