# What Meta's REAL compositor shows each eye, measured without wearing the headset (docs/DISPLAY.md
# section 8). Needs the game in a world with F9 on, through Link (Run-Patrol.ps1 -Runtime meta), and
# the headset awake: on the desk with the proximity sensor covered, or bypassed.
#
# For each case it sets the display switches, opens OculusMirror.exe with both eyes undistorted over
# one shared symmetric field, captures its window and measures the shift between the eyes
# (mirror_eyes.py). In mono the eyes must be identical; any shift is the double vision, in degrees.
#
#   Measure-Mirror.ps1                     # the standard cases: mono, mono with the old submission,
#                                          # the declaration probes, stereo
#   Measure-Mirror.ps1 -Cases mono         # one case
param(
    [string[]]$Cases = @('mono', 'mono_whole', 'probe_yaw', 'probe_fov', 'stereo'),
    [int]$Width = 1800,
    [int]$Height = 900,
    [double[]]$Tan = @(1.3764, 1.4281)
)
$ErrorActionPreference = 'Stop'
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'lib\GameIO.ps1')
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$out = Join-Path $root 'build\shots\mirror'
if (-not (Test-Path $out)) { New-Item -ItemType Directory -Force -Path $out | Out-Null }
$game = 'C:\Games\Sunrise'
$mirrorExe = 'C:\Program Files\Meta Horizon\Support\oculus-diagnostics\OculusMirror.exe'
if (-not (Test-Path $mirrorExe)) { throw "OculusMirror not found at $mirrorExe" }
$enc = New-Object System.Text.UTF8Encoding($false)
$inv = [System.Globalization.CultureInfo]::InvariantCulture

Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices;
public struct MRECT { public int L, T, R, B; }
public struct MPOINT { public int X, Y; }
public static class MirrorWin {
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out MRECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref MPOINT p);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
}
'@

function Send-Display([string[]]$Lines) {
    $path = Join-Path $game 'SVR_Display.txt'
    [System.IO.File]::WriteAllText($path, (($Lines -join "`n") + "`n"), $enc)
    $deadline = (Get-Date).AddSeconds(5)
    while ((Test-Path $path) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 100 }
}

function Capture-Mirror([string]$Name) {
    Get-Process OculusMirror -ErrorAction SilentlyContinue | Stop-Process -Force
    $mirrorArgs = @('--RectilinearBothEyes', '--SymmetricFov', '--Size', "$Width", "$Height")
    $p = Start-Process -FilePath $mirrorExe -ArgumentList $mirrorArgs -PassThru
    $deadline = (Get-Date).AddSeconds(15)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 300
        $p.Refresh()
        if ($p.MainWindowHandle -ne [IntPtr]::Zero) { break }
    }
    if ($p.MainWindowHandle -eq [IntPtr]::Zero) { throw 'the mirror window never appeared' }
    [void][MirrorWin]::ShowWindow($p.MainWindowHandle, 9)
    [void][MirrorWin]::SetForegroundWindow($p.MainWindowHandle)
    Start-Sleep -Seconds 2
    $r = New-Object MRECT
    [void][MirrorWin]::GetClientRect($p.MainWindowHandle, [ref]$r)
    $o = New-Object MPOINT
    [void][MirrorWin]::ClientToScreen($p.MainWindowHandle, [ref]$o)
    $w = $r.R - $r.L; $h = $r.B - $r.T
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $gfx = [System.Drawing.Graphics]::FromImage($bmp)
    $gfx.CopyFromScreen($o.X, $o.Y, 0, 0, $bmp.Size)
    $gfx.Dispose()
    $path = Join-Path $out "$Name.png"
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    return $path
}

function Measure([string]$Path, [string]$Against = '') {
    $tx = $Tan[0].ToString($inv); $ty = $Tan[1].ToString($inv)
    $extra = if ($Against) { @('--against', $Against) } else { @() }
    $json = & python (Join-Path $PSScriptRoot 'mirror_eyes.py') $Path --tan $tx $ty --out $out --json @extra
    return ($json | Select-Object -Last 1 | ConvertFrom-Json)
}

function Report([string]$Name, $m) {
    $g = $m.global
    $far = if ($m.far) { "far {0} deg (v {1}, {2} tiles, {3} divergent)" -f $m.far.h_median, $m.far.v_median, $m.far.tiles, $m.far.divergent } else { 'no far tiles' }
    $near = if ($m.near) { "near {0} deg over {1} tiles" -f $m.near.h_median, $m.near.tiles } else { 'no near tiles' }
    Write-Output ('{0,-12} global h {1} deg v {2} deg (response {3}); {4}; {5}' -f $Name, $g.h_deg, $g.v_deg, $g.response, $far, $near)
}

if (-not (Get-GameProcess)) { throw 'the game is not running' }
$results = @{}
foreach ($case in $Cases) {
    switch ($case) {
        'mono'       { Send-Display @('stereo off', 'crop on', 'probe 0 1') }
        'mono_whole' { Send-Display @('stereo off', 'crop off', 'probe 0 1') }
        # A deliberate 10 deg error in the declared pose only: an honouring runtime turns the picture
        # by 10 deg in BOTH eyes (no disparity, the whole view moved), an ignoring one shows nothing.
        'probe_yaw'  { Send-Display @('stereo off', 'crop on', 'probe 10 1') }
        # Declared tangents 20 % wider than the rect: an honouring runtime shrinks the picture.
        'probe_fov'  { Send-Display @('stereo off', 'crop on', 'probe 0 1.2') }
        'stereo'     { Send-Display @('stereo on', 'crop on', 'probe 0 1') }
        default      { throw "unknown case $case" }
    }
    Start-Sleep -Seconds 3
    $path = Capture-Mirror $case
    $m = Measure $path
    $results[$case] = $m
    Report $case $m
    if ($case -like 'probe_*' -and $results.ContainsKey('mono')) {
        # The probe's effect on each eye: this capture against the plain mono one, eye by eye.
        $vs = Measure $path (Join-Path $out 'mono.png')
        $results["$case`_vs_mono"] = $vs
        Write-Output ('{0,-12} vs mono: left eye h {1} v {2} scale {3}; right eye h {4} v {5} scale {6}  (honoured: yaw 10 deg / scale 0.833; ignored: 0 / 1)' -f `
            $case, $vs.against.left.h_deg, $vs.against.left.v_deg, $vs.against.left.scale, $vs.against.right.h_deg, $vs.against.right.v_deg, $vs.against.right.scale)
    }
}
Send-Display @('stereo off', 'crop on', 'probe 0 1')
[void](Focus-Game)
$results | ConvertTo-Json -Depth 6 | Set-Content -Encoding ascii (Join-Path $out 'mirror_results.json')
Write-Output "captures and anaglyphs in $out"
