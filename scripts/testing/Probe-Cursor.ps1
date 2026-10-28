# Where does the game draw its pointer for a given position of the system pointer, with the
# eye-shaped render (a tall back buffer squashed into the desktop window)? Boots with VR on, reaches
# character select, aims the mock controller away from the screen (so the laser moves nothing), then
# puts the system pointer at a few known points of the client area with an absolute SendInput move and
# takes a desktop screenshot of each. `cursor_find.py` locates the game's arrow by differencing.
param([ValidateSet('eye', 'desktop')][string]$Shape = 'eye', [string]$Tag = 'cursor')
$ErrorActionPreference = 'Stop'
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'lib\GameIO.ps1')
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$shots = Join-Path $repo 'build\shots'
$game = 'C:\Games\Sunrise'
Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices;
public static class SVRAbs {
    [StructLayout(LayoutKind.Sequential)] public struct MOUSEINPUT { public int dx, dy; public uint data, flags, time; public IntPtr extra; }
    [StructLayout(LayoutKind.Sequential)] public struct INPUT { public uint type; public MOUSEINPUT mi; }
    [DllImport("user32.dll")] public static extern uint SendInput(uint n, INPUT[] inputs, int size);
    [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
    public static void MoveTo(int x, int y) {
        int left = GetSystemMetrics(76), top = GetSystemMetrics(77), w = GetSystemMetrics(78) - 1, h = GetSystemMetrics(79) - 1;
        INPUT i = new INPUT(); i.type = 0;
        i.mi.dx = (int)Math.Round((x - left) * 65535.0 / w); i.mi.dy = (int)Math.Round((y - top) * 65535.0 / h);
        i.mi.flags = 0x0001 | 0x8000 | 0x4000;
        SendInput(1, new INPUT[] { i }, Marshal.SizeOf(typeof(INPUT)));
    }
}
'@
Clear-MockInput
Stop-Game
[System.IO.File]::WriteAllText((Join-Path $game 'SVR_Display.txt'), "shape $Shape`n", (New-Object System.Text.UTF8Encoding($false)))
Remove-Item (Join-Path $game 'SVR_Debug.txt') -ErrorAction SilentlyContinue
$manifest = Enable-MockXr -StartOn
Start-Game -Fresh | Out-Null
$p = Wait-GameWindow -TimeoutSeconds 240
Start-Sleep -Seconds 60
[void](Focus-Game)
# The laser away from the screen: it moves nothing while this probe moves the pointer.
Set-MockInput -HandYaw 90 | Out-Null
Send-GameKey RETURN
Start-Sleep -Seconds 40
[void](Focus-Game)
Export-ClientRect | Out-Null
Write-Output "client: $env:SVR_CLIENT_RECT"
$points = @(@(300, 250), @(1500, 250), @(300, 850), @(1500, 850), @(960, 540))
$i = 0
foreach ($pt in $points) {
    [SVRAbs]::MoveTo($pt[0], $pt[1]); Start-Sleep -Milliseconds 300
    [SVRAbs]::MoveTo($pt[0] + 2, $pt[1] + 1); Start-Sleep -Milliseconds 700
    Save-Shot (Join-Path $shots ("{0}_{1}.png" -f $Tag, $i)) | Out-Null
    Write-Output ("shot {0}: system pointer at {1},{2}" -f $i, ($pt[0] + 2), ($pt[1] + 1))
    $i++
}
