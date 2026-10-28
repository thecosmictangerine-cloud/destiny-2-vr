# The whole life of a VR session on the mock, the way a player lives it (docs/PLAYABLE.md):
# the game launched with VR already on; the title, character select and orbit on the virtual screen;
# the Director driven with the LASER (pointer + trigger, no mouse, no keyboard); the travel on the
# screen; the world immersive; then the runtime misbehaving -- the headset taken off and put back, a
# runtime that stalls, a lost Link, a recentre from the Meta button -- and the game must live through
# all of it. Every check is from the log or the mock's own compositor, never the desktop.
#
#   -Shape desktop   the click coordinates of the Director are the 1920x1080 ones (default);
#                    `eye` runs the same with the eye-shaped render (positions found by -Find).
#   -SkipEvents      stop after landing
param(
    [ValidateSet('desktop', 'eye')][string]$Shape = 'desktop',
    [string]$Destination = 'eden_freeroam 20 160 7',
    [switch]$SkipEvents,
    # After landing, open and close each menu with the keyboard and log the state signals around it.
    [switch]$MenuProbe,
    [string]$Tag = 'life',
    # Character select rows (y 455, 567, 679). Sunrise keeps them in a FIXED order -- on this account
    # Hunter, Titan, Warlock -- whatever was played last (seen 2026-10-18, against the earlier handoff note).
    [ValidateRange(1, 3)][int]$CharacterRow = 1,
    # Words for SVR_Debug.txt (vr_debug.h; e.g. 'stale player'), read once at the first present: the file
    # is removed again as soon as VR is up, so it never reaches a later launch.
    [string]$DebugWords = '',
    [int]$BootSeconds = 240,
    [int]$WorldTimeoutSeconds = 180
)
$ErrorActionPreference = 'Stop'
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'lib\GameIO.ps1')
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$shots = Join-Path $repo 'build\shots'
if (-not (Test-Path $shots)) { New-Item -ItemType Directory -Force -Path $shots | Out-Null }
$game = 'C:\Games\Sunrise'
$log = Join-Path $game 'bin\x64\Sunrise\logs\sunrise.log'
$mockLog = Join-Path $game 'SVR_MockXR.log'
$inv = [System.Globalization.CultureInfo]::InvariantCulture
$results = New-Object System.Collections.ArrayList

function Read-Shared([string]$Path) {
    try {
        $fs = [IO.File]::Open($Path, 'Open', 'Read', 'ReadWrite')
        $sr = New-Object IO.StreamReader($fs)
        $text = $sr.ReadToEnd()
        $sr.Close()
        return $text -split "`n"
    } catch { return @() }
}
function Log-Lines([string]$Pattern) { return @(Read-Shared $log | Where-Object { $_ -match $Pattern }) }
function Log-Last([string]$Pattern) { return @(Log-Lines $Pattern | Select-Object -Last 1) }
function Log-Count([string]$Pattern) { return @(Log-Lines $Pattern).Count }
function Line-Time([string]$Line) { if ($Line -match ' t=(\d+) ') { return [int64]$Matches[1] } return [int64]-1 }
function Log-Now { $l = @(Read-Shared $log | Where-Object { $_ -match ' t=\d+ ' } | Select-Object -Last 1); return Line-Time $l[0] }
function Wait-Log([string]$Pattern, [int]$Seconds, [int64]$After = -1) {
    $deadline = (Get-Date).AddSeconds($Seconds)
    while ((Get-Date) -lt $deadline) {
        $hit = @(Log-Lines $Pattern | Where-Object { (Line-Time $_) -gt $After } | Select-Object -Last 1)
        if ($hit.Count -gt 0) { return $hit[0] }
        if (-not (Get-GameProcess)) { return $null }
        Start-Sleep -Milliseconds 500
    }
    return $null
}
function Check([string]$Name, [bool]$Ok, [string]$Detail) {
    $verdict = if ($Ok) { 'PASS' } else { 'FAIL' }
    [void]$results.Add([pscustomobject]@{ Check = $Name; Verdict = $verdict; Detail = $Detail })
    Write-Output ("{0,-4} {1,-34} {2}" -f $verdict, $Name, $Detail)
}
function Hourglasses { return @(Read-Shared $mockLog | Where-Object { $_ -match 'HOURGLASS #' }).Count }
function Stalls { return Log-Count 'stage=stall result=detected' }

# ---- the pointer -------------------------------------------------------------------------------
# The screen's placement comes from the module's own status line (LOCAL space): centre, yaw, pitch,
# size. The mock's right hand sits at (0.2, 1.4, -0.3) + HandX/Y/Z and points along yaw (+ left) and
# pitch (+ up). Point-At aims it at a UV of the panel.
$script:hand = @{ X = 0.0; Y = 0.0; Z = 0.0 }
function Screen-Status {
    $l = @(Log-Last 'ev=vr\.screen status open=1')
    if ($l.Count -eq 0) { return $null }
    if ($l[0] -notmatch 'yaw=(-?[\d.]+) pitch=(-?[\d.]+) centre=(-?[\d.]+),(-?[\d.]+),(-?[\d.]+) size=([\d.]+),([\d.]+)') { return $null }
    return @{ Yaw = [double]::Parse($Matches[1], $inv) * [Math]::PI / 180; Pitch = [double]::Parse($Matches[2], $inv) * [Math]::PI / 180
              Cx = [double]::Parse($Matches[3], $inv); Cy = [double]::Parse($Matches[4], $inv); Cz = [double]::Parse($Matches[5], $inv)
              W = [double]::Parse($Matches[6], $inv); H = [double]::Parse($Matches[7], $inv); Time = Line-Time $l[0] }
}
function Aim([double]$U, [double]$V, [double]$Trigger = 0, [double]$A = 0, [double]$B = 0, [double]$TurnY = 0, [double]$ThumbR = 0, [double]$MenuBtn = 0) {
    $s = Screen-Status
    if ($null -eq $s) { throw 'no screen status in the log' }
    $ry = $s.Yaw; $rp = $s.Pitch
    # Panel axes: R = (cos y, 0, -sin y), U = (sin p sin y, cos p, sin p cos y).
    $rx = [Math]::Cos($ry); $rz = -[Math]::Sin($ry)
    $ux = [Math]::Sin($rp) * [Math]::Sin($ry); $uy = [Math]::Cos($rp); $uz = [Math]::Sin($rp) * [Math]::Cos($ry)
    $tx = $s.Cx + ($U - 0.5) * $s.W * $rx + (0.5 - $V) * $s.H * $ux
    $ty = $s.Cy + (0.5 - $V) * $s.H * $uy
    $tz = $s.Cz + ($U - 0.5) * $s.W * $rz + (0.5 - $V) * $s.H * $uz
    $hx = 0.2 + $script:hand.X; $hy = 1.4 + $script:hand.Y; $hz = -0.3 + $script:hand.Z
    $dx = $tx - $hx; $dy = $ty - $hy; $dz = $tz - $hz
    $yaw = [Math]::Atan2(-$dx, -$dz) * 180 / [Math]::PI
    $pitch = [Math]::Atan2($dy, [Math]::Sqrt($dx * $dx + $dz * $dz)) * 180 / [Math]::PI
    [void](Set-MockInput -HandYaw $yaw -HandPitch $pitch -HandX $script:hand.X -HandY $script:hand.Y -HandZ $script:hand.Z `
        -TrigR $Trigger -BtnA $A -BtnB $B -TurnY $TurnY -ThumbR $ThumbR -Menu $MenuBtn)
}
# Point, let the hover settle, pull the trigger, let go: a click as a player makes it.
# Design coordinates (the 1920x1080 desktop) to panel UV. With the eye-shaped render the UI canvas
# is 16:10 (1920x1200 in design units), the 16:9 layout centred in it.
function Laser-Click([double]$X, [double]$Y, [double]$Settle = 1.3) {
    $u = $X / 1920.0
    $v = if ($Shape -eq 'eye') { ($Y + 60.0) / 1200.0 } else { $Y / 1080.0 }
    Aim $u $v
    Start-Sleep -Milliseconds 400
    Aim ($u + 0.002) ($v + 0.002)
    Start-Sleep -Seconds $Settle
    Aim ($u + 0.002) ($v + 0.002) -Trigger 1
    Start-Sleep -Milliseconds 150
    Aim ($u + 0.002) ($v + 0.002) -Trigger 0
}
function Compose-Eyes([string]$Name) {
    Get-ChildItem $game -Filter "SVR_seen_$Name*" -ErrorAction SilentlyContinue | Remove-Item -Force
    [System.IO.File]::WriteAllText((Join-Path $game 'SVR_MockCompose.txt'), "$Name`n", (New-Object System.Text.UTF8Encoding($false)))
    $deadline = (Get-Date).AddSeconds(30)
    while (-not (Test-Path (Join-Path $game "SVR_seen_$Name.txt"))) {
        if ((Get-Date) -gt $deadline) { return $null }
        Start-Sleep -Milliseconds 200
    }
    Start-Sleep -Milliseconds 300
    Get-ChildItem $game -Filter "SVR_seen_$Name*" | Copy-Item -Destination $shots -Force
    & python (Join-Path $PSScriptRoot 'eye_view.py') $shots $Name --out $shots --json | Out-Null
    return (Get-Content (Join-Path $game "SVR_seen_$Name.txt") -Raw)
}
function Mock-Event([string]$Text) {
    [System.IO.File]::WriteAllText((Join-Path $game 'SVR_MockEvent.txt'), "$Text`n", (New-Object System.Text.UTF8Encoding($false)))
}

# ---- launch: VR on from the first present ------------------------------------------------------
Clear-MockInput
Stop-Game
Remove-Item $mockLog, (Join-Path $game 'SVR_Weapon.txt'), (Join-Path $game 'SVR_MockEvent.txt') -ErrorAction SilentlyContinue
[System.IO.File]::WriteAllText((Join-Path $game 'SVR_Destination.txt'), "$Destination`n", (New-Object System.Text.UTF8Encoding($false)))
[System.IO.File]::WriteAllText((Join-Path $game 'SVR_Display.txt'), "shape $Shape`n", (New-Object System.Text.UTF8Encoding($false)))
if ($MenuProbe -or $DebugWords) {
    $words = (@($(if ($MenuProbe) { 'stateprobe' }), $DebugWords) | Where-Object { $_ }) -join "`n"
    [System.IO.File]::WriteAllText((Join-Path $game 'SVR_Debug.txt'), "$words`n", (New-Object System.Text.UTF8Encoding($false)))
} else {
    Remove-Item (Join-Path $game 'SVR_Debug.txt') -ErrorAction SilentlyContinue
}
$manifest = Enable-MockXr -StartOn
Write-Output "tag=$Tag shape=$Shape mock=$manifest"
Start-Game -Fresh | Out-Null
$p = Wait-GameWindow -TimeoutSeconds $BootSeconds
if (-not $p) { throw 'no game window' }
$init = Wait-Log 'ev=vr\.xr init result=ok' 120
Check 'vr up without F9' ($null -ne $init) "$init"
if ($DebugWords -and -not $MenuProbe) { Remove-Item (Join-Path $game 'SVR_Debug.txt') -ErrorAction SilentlyContinue }
$screenUp = Wait-Log 'ev=vr\.screen open' 60
Check 'title on the screen' ($null -ne $screenUp) "$screenUp"
Start-Sleep -Seconds 50
[void](Focus-Game)
Set-MockInput | Out-Null
Start-Sleep -Seconds 2
$seen = Compose-Eyes "${Tag}_title"
# The panel and the pointer's beam: two quads over the room.
Check 'title composed' ($null -ne $seen -and $seen -match 'quads [1-9] drawn [1-9]') "quads line: $(($seen -split "`n" | Where-Object { $_ -match '^quads' }) -join ' ')"

# The title: a click anywhere (the laser at the centre).
Laser-Click 960 700 0.5
Start-Sleep -Seconds 40
Save-Shot (Join-Path $shots "${Tag}_charsel.png") | Out-Null
[void](Compose-Eyes "${Tag}_charsel")
# Character select: the first character, with the laser.
Laser-Click 1360 (455 + 112 * ($CharacterRow - 1))
$orbit = Wait-Log "Entering state 'setup:orbit'" 120
Check 'character select by laser' ($null -ne $orbit) "$orbit"
Start-Sleep -Seconds 25
[void](Compose-Eyes "${Tag}_orbit")
Save-Shot (Join-Path $shots "${Tag}_orbit.png") | Out-Null
$servo = Log-Count 'ev=vr\.body servo=stuck'
Check 'no servo in orbit' ($servo -eq 0) "servo=stuck lines: $servo"

# ---- the Director with the laser ---------------------------------------------------------------
$before = Log-Count "Entering state 'activity:in_world'"
$hg0 = Hourglasses
Laser-Click 960 848 2.0; Start-Sleep 5          # OPEN DIRECTOR
Save-Shot (Join-Path $shots "${Tag}_dir1.png") | Out-Null
Laser-Click 960 250; Start-Sleep 6              # Tower
Laser-Click 908 290; Start-Sleep 3              # landing node
Save-Shot (Join-Path $shots "${Tag}_dir3.png") | Out-Null
Laser-Click 1587 884; Start-Sleep 5             # LAUNCH
$launch = Wait-Log "Entering state 'setup:activity_session_creation'" 30
Check 'launch by laser' ($null -ne $launch) "$launch"
Start-Sleep -Seconds 12
[void](Compose-Eyes "${Tag}_travel")
$world = Wait-Log "Entering state 'activity:in_world'" $WorldTimeoutSeconds
Check 'landed' ($null -ne $world) "$world"
$immersive = Wait-Log 'ev=vr\.state view=immersive' 20
Check 'world is immersive' ($null -ne $immersive) "$immersive"
Start-Sleep -Seconds 25
# Only after landing: the engine stalls ~8 s compiling shaders at every boot, and recovers.
$landedAt = if ($world) { Line-Time $world } else { [int64]0 }
function LateStalls { return @(Log-Lines 'stage=stall result=detected' | Where-Object { (Line-Time $_) -gt $landedAt }).Count }
$afterLanding = LateStalls
Check 'no hang after landing' ($afterLanding -eq 0) "stall lines after landing: $afterLanding (boot stalls: $(Stalls))"
Check 'no hourglass through the load' ((Hourglasses) -eq $hg0) "hourglasses: $((Hourglasses) - $hg0)"
$weapon = Log-Last 'ev=vr\.weapon install result=ok'
Check 'weapon hooked at its call sites' ($null -ne $weapon -and $weapon -match 'call_sites') "$weapon"
$callers = Log-Last 'ev=vr\.weapon callers path=getter'
Check 'weapon getter drives the gun' ($callers -match 'D5D832:[1-9]') "$callers"
[void](Focus-Game)
Set-MockInput -HandY 0.25 -HandZ -0.1 | Out-Null
Start-Sleep -Seconds 2
[void](Compose-Eyes "${Tag}_world")
Save-Shot (Join-Path $shots "${Tag}_world.png") | Out-Null

if ($MenuProbe) {
    $marks = New-Object System.Collections.ArrayList
    # Full-screen menus put the screen up; overlays drawn over the world (Esc, Tab) do not.
    $fullScreen = @{ 'F1' = $true; 'ESCAPE' = $false; 'TAB' = $false; 'M' = $true; 'I' = $true }
    foreach ($key in @('F1', 'ESCAPE', 'TAB', 'M', 'I')) {
        [void](Focus-Game)
        $t0 = Log-Now
        [void]$marks.Add("open $key t=$t0")
        Send-GameKey $key
        Start-Sleep -Seconds 4
        $up = @(Log-Lines 'ev=vr\.state view=screen' | Where-Object { (Line-Time $_) -gt $t0 }).Count -gt 0
        if ($fullScreen[$key]) {
            Check "menu $key on the screen" $up "screen opened: $up"
            [void](Compose-Eyes "${Tag}_menu_$key")
        } else {
            Check "overlay $key stays in the world" (-not $up) "screen opened: $up"
        }
        Save-Shot (Join-Path $shots "${Tag}_menu_$key.png") | Out-Null
        $t1 = Log-Now
        [void]$marks.Add("close $key t=$t1")
        Send-GameKey ESCAPE
        Start-Sleep -Seconds 4
        if ($fullScreen[$key]) {
            $back = @(Log-Lines 'ev=vr\.state view=immersive' | Where-Object { (Line-Time $_) -gt $t1 }).Count -gt 0
            Check "menu $key closed: world" $back "immersive again: $back"
        }
        $flips = @(Log-Lines 'ev=vr\.state view=' | Where-Object { (Line-Time $_) -gt $t0 }).Count
        Check "menu $key no flicker" ($flips -le 2) "view changes: $flips"
        [void]$marks.Add("world $key t=$(Log-Now)")
        Save-Shot (Join-Path $shots "${Tag}_menu_after_$key.png") | Out-Null
        Start-Sleep -Seconds 2
    }
    # The controllers: the menu button tapped is the character screen, held the Director; B closes.
    # Milliseconds: PowerShell 5.1's -Seconds is an integer, and 0.15 s rounded to a press of nothing.
    foreach ($case in @(@('tap', 180), @('hold', 1000))) {
        [void](Focus-Game)
        $t0 = Log-Now
        Set-MockInput -HandY 0.25 -HandZ -0.1 -Menu 1 | Out-Null
        Start-Sleep -Milliseconds $case[1]
        Set-MockInput -HandY 0.25 -HandZ -0.1 -Menu 0 | Out-Null
        Start-Sleep -Seconds 4
        $up = @(Log-Lines 'ev=vr\.state view=screen' | Where-Object { (Line-Time $_) -gt $t0 }).Count -gt 0
        Check "menu button $($case[0]): screen" $up "screen opened: $up"
        Save-Shot (Join-Path $shots "${Tag}_button_$($case[0]).png") | Out-Null
        [void](Compose-Eyes "${Tag}_button_$($case[0])")
        $t1 = Log-Now
        Set-MockInput -HandY 0.25 -HandZ -0.1 -BtnB 1 | Out-Null
        Start-Sleep -Milliseconds 200
        Set-MockInput -HandY 0.25 -HandZ -0.1 -BtnB 0 | Out-Null
        Start-Sleep -Seconds 4
        $back = @(Log-Lines 'ev=vr\.state view=immersive' | Where-Object { (Line-Time $_) -gt $t1 }).Count -gt 0
        Check "B closes it: world" $back "immersive again: $back"
    }
    $marks | Set-Content -Path (Join-Path $shots "${Tag}_marks.txt")
    $marks | ForEach-Object { Write-Output "MARK $_" }
}

if (-not $SkipEvents) {
    # ---- the runtime misbehaves ----------------------------------------------------------------
    # The headset off and back on: the session stops and comes back, the game never stops.
    $t0 = Log-Now
    Mock-Event 'stop 4'
    $stopped = Wait-Log 'ev=vr\.xr session state=stopped' 15 $t0
    $running = Wait-Log 'ev=vr\.xr session state=running' 20 $t0
    Check 'headset off and on' ($null -ne $stopped -and $null -ne $running) "stopped=$([bool]$stopped) running=$([bool]$running)"
    $resizes = @(Log-Lines 'ev=vr\.display request target' | Where-Object { (Line-Time $_) -gt $t0 }).Count
    Check 'no resize on headset off/on' ($resizes -eq 0) "resize requests: $resizes"

    # The headset off for longer than the kick (5 s) and the old session never offered back -- the
    # 18 Oct Quest session (Air Link): the module tears the session down and makes a new one, which must
    # be created on the present thread (on the init thread it froze the game for good on Meta).
    $t0 = Log-Now
    Mock-Event 'stop'
    $kick = Wait-Log 'ev=vr\.xr session not_offered_back action=new_session' 20 $t0
    $made = Wait-Log 'ev=vr\.xr session result=ok .*thread=present' 30 $t0
    $running3 = Wait-Log 'ev=vr\.xr session state=running' 30 $t0
    Check 'headset off past the kick: new session' ($null -ne $kick -and $null -ne $made -and $null -ne $running3) "kick=$([bool]$kick) created=$([bool]$made) running=$([bool]$running3) $made"
    Start-Sleep -Seconds 4
    # From the landing on, as every check below: a boot stall (shaders) is not the events' doing.
    Check 'no hang after the new session' ((LateStalls) -eq 0) "stall lines after landing: $(LateStalls)"

    # A runtime that holds xrWaitFrame for 3 s: the game goes on (the wait is bounded).
    $t0 = Log-Now
    Mock-Event 'stall 3'
    Start-Sleep -Seconds 6
    $bound = Wait-Log 'ev=vr\.xr wait_bound ms=' 5 $t0
    $quiet = Log-Count 'ev=vr\.xr game_quiet'
    Check 'runtime stall: game goes on' ($null -ne $bound -and (LateStalls) -eq 0) "bound=$([bool]$bound) game_quiet lines=$quiet"

    # A recentre from the runtime's menu: our origin follows.
    $t0 = Log-Now
    Mock-Event 'recentre'
    $rc = Wait-Log 'ev=vr\.xr recentre reason=runtime' 10 $t0
    Check 'runtime recentre followed' ($null -ne $rc) "$rc"

    # Link lost: everything torn down and back on its own.
    $t0 = Log-Now
    Mock-Event 'loss'
    $teardown = Wait-Log 'ev=vr\.xr restart action=teardown' 15 $t0
    $again = Wait-Log 'ev=vr\.xr init result=ok' 30 $t0
    $running2 = Wait-Log 'ev=vr\.xr session state=running' 30 $t0
    Check 'Link lost and back' ($null -ne $teardown -and $null -ne $again -and $null -ne $running2) "teardown=$([bool]$teardown) init=$([bool]$again) running=$([bool]$running2)"
    Start-Sleep -Seconds 5
    Check 'still no hang' ((LateStalls) -eq 0) "stall lines after landing: $(LateStalls)"
    [void](Compose-Eyes "${Tag}_after")
}

$fails = @($results | Where-Object { $_.Verdict -eq 'FAIL' }).Count
Write-Output ("SUMMARY {0}/{1} passed" -f ($results.Count - $fails), $results.Count)
Copy-Item $log (Join-Path $shots "${Tag}_sunrise.log") -Force
Copy-Item $mockLog (Join-Path $shots "${Tag}_mock.log") -Force -ErrorAction SilentlyContinue
