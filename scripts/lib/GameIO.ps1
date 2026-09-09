# Shared helpers for driving the game headless: launch, focus, synthetic input, screenshots.
#
# Injected input goes through keybd_event/mouse_event with SCANCODES, which is what the game's
# raw-input path reads; WM_-level fakes (SendKeys) are ignored by it. Screenshots are taken
# DPI-aware, or a scaled desktop silently crops the capture to the top-left of the screen.

$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Windows.Forms, System.Drawing

if (-not ('SunriseVR.Native' -as [type])) {
    Add-Type -Namespace SunriseVR -Name Native -MemberDefinition @'
[DllImport("user32.dll")] public static extern void keybd_event(byte bVk, byte bScan, uint dwFlags, IntPtr dwExtraInfo);
[DllImport("user32.dll")] public static extern void mouse_event(uint dwFlags, uint dx, uint dy, uint dwData, IntPtr dwExtraInfo);
[DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
[DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, IntPtr pid);
[DllImport("user32.dll")] public static extern bool AttachThreadInput(uint idAttach, uint idAttachTo, bool fAttach);
[DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr hWnd);
[DllImport("user32.dll")] public static extern int GetWindowTextW(IntPtr hWnd, System.Text.StringBuilder s, int n);
[DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
'@
}

[void][SunriseVR.Native]::SetProcessDPIAware()

# Virtual key and scancode for every key the test steps name.
$script:Keys = @{
    'RETURN' = @(0x0D, 0x1C, $false)
    'ESCAPE' = @(0x1B, 0x01, $false)
    'SPACE'  = @(0x20, 0x39, $false)
    'INSERT' = @(0x2D, 0x52, $true)
    'TAB'    = @(0x09, 0x0F, $false)
    'F8'     = @(0x77, 0x42, $false)
    'F9'     = @(0x78, 0x43, $false)
    'F10'    = @(0x79, 0x44, $false)
    'F11'    = @(0x7A, 0x57, $false)
    'W'      = @(0x57, 0x11, $false)
    'A'      = @(0x41, 0x1E, $false)
    'S'      = @(0x53, 0x1F, $false)
    'D'      = @(0x44, 0x20, $false)
    'E'      = @(0x45, 0x12, $false)
}

# Scancodes for the characters Send-GameText can type. Enough for activity package names.
$script:CharScan = @{
    'a' = 0x1E; 'b' = 0x30; 'c' = 0x2E; 'd' = 0x20; 'e' = 0x12; 'f' = 0x21; 'g' = 0x22
    'h' = 0x23; 'i' = 0x17; 'j' = 0x24; 'k' = 0x25; 'l' = 0x26; 'm' = 0x32; 'n' = 0x31
    'o' = 0x18; 'p' = 0x19; 'q' = 0x10; 'r' = 0x13; 's' = 0x1F; 't' = 0x14; 'u' = 0x16
    'v' = 0x2F; 'w' = 0x11; 'x' = 0x2D; 'y' = 0x15; 'z' = 0x2C
    '0' = 0x0B; '1' = 0x02; '2' = 0x03; '3' = 0x04; '4' = 0x05; '5' = 0x06; '6' = 0x07
    '7' = 0x08; '8' = 0x09; '9' = 0x0A; '_' = 0x0C; '-' = 0x0C; ' ' = 0x39
}

$script:GamePath = 'C:\Games\Sunrise\destiny2.exe'
$script:GameDir = 'C:\Games\Sunrise'

function Get-GameProcess {
    Get-Process -Name destiny2 -ErrorAction SilentlyContinue | Select-Object -First 1
}

function Stop-Game {
    Get-Process -Name destiny2 -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Seconds 3
}

function Start-Game {
    param([switch]$Fresh)
    if ($Fresh) { Stop-Game }
    if (Get-GameProcess) { return Get-GameProcess }
    Start-Process -FilePath $script:GamePath -WorkingDirectory $script:GameDir
    return $null
}

<#
Waits for the game to put up its window. The first launch after an install extracts files and
can sit there for minutes, so the caller owns the budget.
#>
function Wait-GameWindow {
    param([int]$TimeoutSeconds = 180)
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 5
        $p = Get-GameProcess
        if (-not $p) { return $null }
        if ($p.MainWindowHandle -ne 0 -and $p.MainWindowTitle) { return $p }
    }
    return $null
}

function Focus-Game {
    $p = Get-GameProcess
    if (-not $p) { return $false }
    [void][SunriseVR.Native]::ShowWindow($p.MainWindowHandle, 9)
    [void][SunriseVR.Native]::SetForegroundWindow($p.MainWindowHandle)
    Start-Sleep -Milliseconds 800
    return ([SunriseVR.Native]::GetForegroundWindow() -eq $p.MainWindowHandle)
}

<#
Brings the game to the foreground and REFUSES TO CARRY ON if it does not get there.

Focus-Game has always returned whether it succeeded, and every call site discarded that with
`[void]`. The cost of that showed up as a screenshot of the editor instead of the game: the
template matcher then compared the weapon against a code window, reported a 582 px shift at a
correlation peak of 0.04, and a measurement that was pure garbage came back looking like a number.
A capture of the wrong window must be a loud failure, never a quiet one.

@param Attempts How many times to try before giving up.
@param Quiet Return false instead of throwing.
#>
function Assert-GameFocus {
    param([int]$Attempts = 8, [switch]$Quiet)
    for ($i = 0; $i -lt $Attempts; $i++) {
        if (Focus-Game) { return $true }
        # Windows grants SetForegroundWindow only to a process that already holds the foreground or
        # has recently received input, and a script host has neither -- so the plain call succeeds
        # when the game happens to be in front already and is silently refused otherwise, which is
        # exactly the intermittent pattern seen here. Borrowing the foreground thread's input queue
        # for the duration of the call is the documented way round it.
        if (Focus-GameHard) { return $true }
        Start-Sleep -Milliseconds 600
    }
    if ($Quiet) { return $false }
    throw ('the game is not in the foreground -- refusing to capture, the shot would be of another ' +
        "window. In front instead: '" + (Get-ForegroundTitle) + "'")
}

<# @return The title of whatever window currently holds the foreground, for diagnosing a lost race. #>
function Get-ForegroundTitle {
    $builder = New-Object System.Text.StringBuilder 256
    [void][SunriseVR.Native]::GetWindowTextW([SunriseVR.Native]::GetForegroundWindow(), $builder, 256)
    return $builder.ToString()
}

<# Focus-Game, but attaching to the foreground thread's input queue first. See Assert-GameFocus. #>
function Focus-GameHard {
    $p = Get-GameProcess
    if (-not $p) { return $false }
    $target = $p.MainWindowHandle
    $foreground = [SunriseVR.Native]::GetForegroundWindow()
    $theirThread = [SunriseVR.Native]::GetWindowThreadProcessId($foreground, [IntPtr]::Zero)
    $myThread = [SunriseVR.Native]::GetCurrentThreadId()
    $attached = $false
    if ($theirThread -ne 0 -and $theirThread -ne $myThread) {
        $attached = [SunriseVR.Native]::AttachThreadInput($myThread, $theirThread, $true)
    }
    try {
        [void][SunriseVR.Native]::ShowWindow($target, 9)
        [void][SunriseVR.Native]::BringWindowToTop($target)
        [void][SunriseVR.Native]::SetForegroundWindow($target)
    } finally {
        if ($attached) { [void][SunriseVR.Native]::AttachThreadInput($myThread, $theirThread, $false) }
    }
    Start-Sleep -Milliseconds 500
    return ([SunriseVR.Native]::GetForegroundWindow() -eq $target)
}

<#
Fires the weapon once, to take it out of its idle pose before anything is measured.

The user's note, and it is a real hazard for every weapon measurement: after a while without input the
weapon settles into an idle animation that points it upwards, and one click of the fire button puts
it back to the normal pose -- level with the ground when the view is level. Measuring in one pose
and comparing against the other would be a large, silent error that correlates with how long the
session has been sitting rather than with anything under test.

Measured on this build: with the weapon already out of idle, twenty seconds of standing still moves
it 0 px and firing moves it 0 px (ammo 13 -> 12, so the shot did happen). So this is cheap
insurance, not a workaround for a moving target.

The click goes through mouse_event without moving the cursor, because the game reads raw input and
because moving the pointer would turn the view.
#>
function Clear-WeaponIdle {
    param([int]$SettleMs = 1200)
    if (-not (Assert-GameFocus -Quiet)) { return $false }
    [SunriseVR.Native]::mouse_event(0x0002, 0, 0, 0, [IntPtr]::Zero)   # LEFTDOWN
    Start-Sleep -Milliseconds 90
    [SunriseVR.Native]::mouse_event(0x0004, 0, 0, 0, [IntPtr]::Zero)   # LEFTUP
    Start-Sleep -Milliseconds $SettleMs
    return $true
}

function Send-GameKey {
    param([Parameter(Mandatory)][string]$Name, [int]$HoldMs = 80)
    $key = $script:Keys[$Name.ToUpper()]
    if (-not $key) { throw "unknown key: $Name" }
    $vk = [byte]$key[0]
    $scan = [byte]$key[1]
    # KEYEVENTF_SCANCODE = 0x08, KEYEVENTF_EXTENDEDKEY = 0x01, KEYEVENTF_KEYUP = 0x02
    $flags = 0x08
    if ($key[2]) { $flags = $flags -bor 0x01 }
    [SunriseVR.Native]::keybd_event($vk, $scan, [uint32]$flags, [IntPtr]::Zero)
    Start-Sleep -Milliseconds $HoldMs
    [SunriseVR.Native]::keybd_event($vk, $scan, [uint32]($flags -bor 0x02), [IntPtr]::Zero)
    Start-Sleep -Milliseconds 120
}

function Send-GameText {
    param([Parameter(Mandatory)][string]$Text)
    foreach ($ch in $Text.ToLower().ToCharArray()) {
        $scan = $script:CharScan["$ch"]
        if (-not $scan) { continue }
        [SunriseVR.Native]::keybd_event(0, [byte]$scan, 0x08, [IntPtr]::Zero)
        Start-Sleep -Milliseconds 50
        [SunriseVR.Native]::keybd_event(0, [byte]$scan, 0x0A, [IntPtr]::Zero)
        Start-Sleep -Milliseconds 90
    }
}

function Send-GameClick {
    param([Parameter(Mandatory)][int]$X, [Parameter(Mandatory)][int]$Y)
    [void][SunriseVR.Native]::SetCursorPos($X, $Y)
    Start-Sleep -Milliseconds 400
    # MOUSEEVENTF_LEFTDOWN = 0x0002, MOUSEEVENTF_LEFTUP = 0x0004
    [SunriseVR.Native]::mouse_event(0x0002, 0, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 90
    [SunriseVR.Native]::mouse_event(0x0004, 0, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 700
}

function Save-Shot {
    param([Parameter(Mandatory)][string]$Path)
    $bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    $bmp = New-Object System.Drawing.Bitmap $bounds.Width, $bounds.Height
    $gfx = [System.Drawing.Graphics]::FromImage($bmp)
    $gfx.CopyFromScreen($bounds.X, $bounds.Y, 0, 0, $bmp.Size)
    $gfx.Dispose()
    $dir = Split-Path -Parent $Path
    if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    $bmp.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    return $Path
}

<#
Runs a comma-separated step list against the running game. Steps:
  sleep:<seconds>  key:<name>  text:<literal>  click:<x>x<y>  focus  shot:<path>
#>
function Invoke-GameSteps {
    param([Parameter(Mandatory)][string[]]$Steps)
    foreach ($step in $Steps) {
        $step = $step.Trim()
        if (-not $step) { continue }
        $verb, $arg = $step -split ':', 2
        switch ($verb.ToLower()) {
            'sleep' { Start-Sleep -Seconds ([double]$arg) }
            'key'   { Send-GameKey -Name $arg }
            'text'  { Send-GameText -Text $arg }
            'focus' { [void](Focus-Game) }
            'shot'  { [void](Save-Shot -Path $arg); Write-Output "shot -> $arg" }
            'click' {
                $x, $y = $arg -split 'x', 2
                Send-GameClick -X ([int]$x) -Y ([int]$y)
            }
            default { throw "unknown step: $step" }
        }
    }
}

function Get-GameStats {
    $p = Get-GameProcess
    if (-not $p) { return 'dead' }
    return ('alive rss={0} responding={1} title="{2}"' -f $p.WorkingSet64, $p.Responding, $p.MainWindowTitle)
}

<#
Writes the mock runtime's synthetic pose file. Every field is written explicitly: the mock keeps
the previous value for anything left out, which makes partial writes stick across steps.
Angles are degrees, positions metres. HeadY is an offset on a 1.6 m standing height.
#>
function Set-MockInput {
    param(
        [double]$HeadYaw = 0, [double]$HeadPitch = 0,
        [double]$HeadX = 0, [double]$HeadY = 0, [double]$HeadZ = 0,
        [double]$MoveX = 0, [double]$MoveY = 0,
        [double]$TurnX = 0, [double]$TurnY = 0,
        [double]$HandYaw = 0, [double]$HandPitch = 0,
        [double]$HandX = 0, [double]$HandY = 0, [double]$HandZ = 0,
        [double]$TrigR = 0, [double]$TrigL = 0,
        [double]$BtnA = 0, [double]$BtnB = 0, [double]$BtnX = 0, [double]$BtnY = 0,
        [double]$GripL = 0, [double]$GripR = 0,
        [double]$ThumbL = 0, [double]$ThumbR = 0, [double]$Menu = 0,
        [double]$LHandYaw = 0, [double]$LHandPitch = 0,
        [double]$LHandX = 0, [double]$LHandY = 0, [double]$LHandZ = 0,
        # Roll is the wrist axis, and the one the weapon's pivot error shows up in. Appended last
        # to match the mock's field order, which only ever grows at the end.
        [double]$HandRoll = 0, [double]$LHandRoll = 0
    )
    $fields = @(
        $TurnX, $TrigR, $HandYaw, $HandPitch, $HeadYaw, $HeadPitch, $HeadX, $HeadY, $HeadZ,
        $MoveX, $MoveY, $HandX, $HandY, $HandZ, $BtnA, $BtnB, $BtnX, $BtnY,
        $TurnY, $TrigL, $GripL, $GripR, $ThumbL, $ThumbR, $Menu,
        $LHandYaw, $LHandPitch, $LHandX, $LHandY, $LHandZ,
        $HandRoll, $LHandRoll
    )
    $text = ($fields | ForEach-Object { $_.ToString([System.Globalization.CultureInfo]::InvariantCulture) }) -join ' '
    $enc = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText((Join-Path $script:GameDir 'SVR_MockInput.txt'), "$text`n", $enc)
    return $text
}

<#
Publishes the game's real client rectangle in SVR_CLIENT_RECT, as "x,y,w,h" in physical pixels,
for the Python image tools to crop with.

They used to carry the rectangle as a constant, which went wrong the moment the window resolution
changed -- and silently, because the crops still produced pictures, just of the wrong part of the
frame. Asking Windows removes the assumption.
#>
function Export-ClientRect {
    Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices;
public struct SVRIORECT { public int L, T, R, B; }
public struct SVRIOPOINT { public int X, Y; }
public static class SVRIOWin {
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out SVRIORECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref SVRIOPOINT p);
}
'@ -ErrorAction SilentlyContinue
    $p = Get-GameProcess
    if (-not $p) { return $null }
    $r = New-Object SVRIORECT
    [void][SVRIOWin]::GetClientRect($p.MainWindowHandle, [ref]$r)
    $o = New-Object SVRIOPOINT
    [void][SVRIOWin]::ClientToScreen($p.MainWindowHandle, [ref]$o)
    $env:SVR_CLIENT_RECT = ('{0},{1},{2},{3}' -f $o.X, $o.Y, ($r.R - $r.L), ($r.B - $r.T))
    return $env:SVR_CLIENT_RECT
}

<# Removes the pose file, putting the mock back on its own procedural yaw sweep. #>
function Clear-MockInput {
    $path = Join-Path $script:GameDir 'SVR_MockInput.txt'
    if (Test-Path $path) { Remove-Item $path -Force }
}

<#
Points the next game launch at the mock OpenXR runtime instead of the real one.

The DLL is copied INTO the game directory first. Loading it from the repo failed inside the game
process with ERROR_DLL_INIT_FAILED (1114) while loading fine from a plain test process, and the
game directory is both on the loader's search path and already excluded from Defender, which
removes two whole classes of cause at once.
#>
function Enable-MockXr {
    $repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
    $built = Join-Path $repo 'build\mockxr\SunriseVR_MockXR.dll'
    if (-not (Test-Path $built)) { throw "mock not built: $built" }
    $deployed = Join-Path $script:GameDir 'SunriseVR_MockXR.dll'
    Copy-Item $built $deployed -Force
    $manifest = Join-Path $script:GameDir 'SunriseVR_MockXR.json'
    $escaped = $deployed -replace '\\', '\\'
    $json = @"
{
  "file_format_version": "1.0.0",
  "runtime": {
    "library_path": "$escaped"
  }
}
"@
    [System.IO.File]::WriteAllText($manifest, $json, (New-Object System.Text.UTF8Encoding($false)))
    $env:XR_RUNTIME_JSON = $manifest
    return $manifest
}
