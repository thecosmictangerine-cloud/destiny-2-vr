<#
Out-of-process memory scanner for the running game.

Dot-source it. Finds where a float triple (a position or a unit vector) lives in destiny2.exe's
private memory, reads arbitrary ranges back as floats, and diffs two dumps. Used to locate the
consumers of the camera pose (the weapon viewmodel among them) without rebuilding the mod.

  $hits = Find-FloatTriple 258.14,258.30,-13.55 -Tolerance 0.006
  $blk  = Read-Floats ($hits[0] - 0x594) 0xC50
  Compare-Dumps $a $b -MinDelta 0.001
#>

Add-Type -TypeDefinition @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace SunriseVR {
public static class Mem {
    [DllImport("kernel32.dll", SetLastError=true)] public static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32.dll", SetLastError=true)] public static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll", SetLastError=true)] public static extern bool ReadProcessMemory(IntPtr h, IntPtr addr, byte[] buf, IntPtr size, out IntPtr read);
    [DllImport("kernel32.dll", SetLastError=true)] public static extern bool WriteProcessMemory(IntPtr h, IntPtr addr, byte[] buf, IntPtr size, out IntPtr written);
    [DllImport("kernel32.dll", SetLastError=true)] public static extern IntPtr VirtualQueryEx(IntPtr h, IntPtr addr, out MBI mbi, IntPtr len);

    [StructLayout(LayoutKind.Sequential)]
    public struct MBI { public IntPtr BaseAddress; public IntPtr AllocationBase; public uint AllocationProtect; public uint __align; public IntPtr RegionSize; public uint State; public uint Protect; public uint Type; public uint __align2; }

    public static IntPtr Open(int pid) { return OpenProcess(0x0438, false, pid); } // QUERY|VM_READ|VM_WRITE|VM_OPERATION

    public static byte[] Read(IntPtr h, long addr, int size) {
        var buf = new byte[size]; IntPtr got;
        if (!ReadProcessMemory(h, (IntPtr)addr, buf, (IntPtr)size, out got)) return null;
        if ((long)got != size) Array.Resize(ref buf, (int)got);
        return buf;
    }

    public static bool Write(IntPtr h, long addr, byte[] data) {
        IntPtr put;
        return WriteProcessMemory(h, (IntPtr)addr, data, (IntPtr)data.Length, out put) && (long)put == data.Length;
    }

    public struct Region { public long Base; public long Size; public uint Protect; public uint Type; }

    public static List<Region> Regions(IntPtr h, bool includeImage) {
        var list = new List<Region>(); long addr = 0; MBI m;
        while ((long)VirtualQueryEx(h, (IntPtr)addr, out m, (IntPtr)Marshal.SizeOf(typeof(MBI))) != 0) {
            long size = (long)m.RegionSize;
            bool committed = m.State == 0x1000;
            bool guard = (m.Protect & 0x100) != 0 || (m.Protect & 0x01) != 0;
            bool readable = (m.Protect & 0x02) != 0 || (m.Protect & 0x04) != 0 || (m.Protect & 0x20) != 0 || (m.Protect & 0x40) != 0;
            bool typeOk = m.Type == 0x20000 || (includeImage && m.Type == 0x1000000);
            if (committed && !guard && readable && typeOk)
                list.Add(new Region { Base = (long)m.BaseAddress, Size = size, Protect = m.Protect, Type = m.Type });
            long next = (long)m.BaseAddress + size;
            if (next <= addr) break;
            addr = next;
            if (addr > 0x7FFFFFFFFFFF) break;
        }
        return list;
    }

    /// Finds 4-byte-aligned float triples within tolerance of (x,y,z).
    public static List<long> FindTriple(IntPtr h, float x, float y, float z, float tol, bool includeImage, int maxHits) {
        var hits = new List<long>();
        foreach (var r in Regions(h, includeImage)) {
            long off = 0;
            while (off < r.Size) {
                int chunk = (int)Math.Min(1 << 22, r.Size - off);
                var buf = Read(h, r.Base + off, chunk);
                if (buf == null) { off += chunk; continue; }
                int n = buf.Length - 12;
                for (int i = 0; i <= n; i += 4) {
                    float a = BitConverter.ToSingle(buf, i);
                    if (!(Math.Abs(a - x) <= tol)) continue;
                    float b = BitConverter.ToSingle(buf, i + 4);
                    if (!(Math.Abs(b - y) <= tol)) continue;
                    float c = BitConverter.ToSingle(buf, i + 8);
                    if (!(Math.Abs(c - z) <= tol)) continue;
                    hits.Add(r.Base + off + i);
                    if (hits.Count >= maxHits) return hits;
                }
                off += chunk;
            }
        }
        return hits;
    }
}
}
"@ -ErrorAction SilentlyContinue

function Get-GameHandle {
    $p = Get-Process -Name destiny2 -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $p) { throw 'game not running' }
    $h = [SunriseVR.Mem]::Open($p.Id)
    if ($h -eq [IntPtr]::Zero) { throw "OpenProcess failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())" }
    return $h
}

<# Returns addresses (as Int64) of float triples matching within Tolerance. #>
function Find-FloatTriple {
    param([Parameter(Mandatory)][float[]]$Values, [float]$Tolerance = 0.002, [switch]$IncludeImage, [int]$MaxHits = 2000)
    $h = Get-GameHandle
    try { return [SunriseVR.Mem]::FindTriple($h, $Values[0], $Values[1], $Values[2], $Tolerance, [bool]$IncludeImage, $MaxHits) }
    finally { [void][SunriseVR.Mem]::CloseHandle($h) }
}

<# Reads Size bytes at Address and returns them as float[] (Size rounded down to 4). #>
function Read-Floats {
    param([Parameter(Mandatory)][long]$Address, [Parameter(Mandatory)][int]$Size)
    $h = Get-GameHandle
    try {
        $buf = [SunriseVR.Mem]::Read($h, $Address, $Size)
        if (-not $buf) { throw ("read failed at 0x{0:X}" -f $Address) }
        $n = [int]($buf.Length / 4)
        $out = New-Object float[] $n
        [Buffer]::BlockCopy($buf, 0, $out, 0, $n * 4)
        return $out
    } finally { [void][SunriseVR.Mem]::CloseHandle($h) }
}

function Read-Bytes {
    param([Parameter(Mandatory)][long]$Address, [Parameter(Mandatory)][int]$Size)
    $h = Get-GameHandle
    try { return [SunriseVR.Mem]::Read($h, $Address, $Size) }
    finally { [void][SunriseVR.Mem]::CloseHandle($h) }
}

function Write-Floats {
    param([Parameter(Mandatory)][long]$Address, [Parameter(Mandatory)][float[]]$Values)
    $bytes = New-Object byte[] ($Values.Length * 4)
    [Buffer]::BlockCopy($Values, 0, $bytes, 0, $bytes.Length)
    $h = Get-GameHandle
    try { return [SunriseVR.Mem]::Write($h, $Address, $bytes) }
    finally { [void][SunriseVR.Mem]::CloseHandle($h) }
}

<# Lists lanes whose value changed between two float dumps. #>
function Compare-Dumps {
    param([Parameter(Mandatory)][float[]]$A, [Parameter(Mandatory)][float[]]$B, [float]$MinDelta = 0.0005, [long]$BaseOffset = 0)
    $n = [Math]::Min($A.Length, $B.Length)
    for ($i = 0; $i -lt $n; $i++) {
        $d = [Math]::Abs($A[$i] - $B[$i])
        if ($d -ge $MinDelta -or ([float]::IsNaN($A[$i]) -ne [float]::IsNaN($B[$i]))) {
            '{0,6} +0x{1:X4}  {2,12:F4} -> {3,12:F4}' -f $i, ($BaseOffset + $i * 4), $A[$i], $B[$i]
        }
    }
}

<# Formats a float dump as offset/value rows, skipping lanes that look like garbage. #>
function Format-Floats {
    param([Parameter(Mandatory)][float[]]$Values, [long]$BaseOffset = 0, [switch]$All)
    for ($i = 0; $i -lt $Values.Length; $i++) {
        $v = $Values[$i]
        $sane = -not [float]::IsNaN($v) -and -not [float]::IsInfinity($v) -and ([Math]::Abs($v) -lt 1e6) -and ($v -eq 0 -or [Math]::Abs($v) -gt 1e-6)
        if ($All -or $sane) { '+0x{0:X4}  {1,14:F5}' -f ($BaseOffset + $i * 4), $v }
    }
}

<#
Asks the running mod to watch one address for a while (needs the build with vr_watch.cpp).
Writes SVR_Watch.txt beside the executable, waits for the window plus a margin, and returns the
new lines of SVR_Watch.log. Mode 'w' traps writes only, 'rw' reads and writes.
#>
function Start-Watch {
    param([Parameter(Mandatory)][long]$Address, [int]$Length = 4, [string]$Mode = 'rw', [int]$Ms = 3000,
          [int]$Slot = 0, [string]$GameDir = 'C:\Games\Sunrise')
    $report = Join-Path $GameDir 'SVR_Watch.log'
    $before = 0
    if (Test-Path $report) { $before = (Get-Content $report).Count }
    $text = ('{0:X} {1} {2} {3} {4}' -f $Address, $Length, $Mode, $Ms, $Slot)
    [System.IO.File]::WriteAllText((Join-Path $GameDir 'SVR_Watch.txt'), "$text`n", (New-Object System.Text.UTF8Encoding($false)))
    $deadline = (Get-Date).AddMilliseconds($Ms + 8000)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 500
        if ((Test-Path $report) -and ((Get-Content $report).Count -gt $before) -and ((Get-Content $report)[-1] -eq 'end')) {
            return (Get-Content $report | Select-Object -Skip $before)
        }
    }
    return "timeout: no report (command file still present: $(Test-Path (Join-Path $GameDir 'SVR_Watch.txt')))"
}

<#
Sends rule lines to the F3 probe (vr_weapon.cpp) and returns the log lines it produced.
  Send-WeaponCommand 'getter all body', 'block off'
  Send-WeaponCommand 'report'
#>
function Send-WeaponCommand {
    param([Parameter(Mandatory)][string[]]$Lines, [string]$GameDir = 'C:\Games\Sunrise', [int]$WaitMs = 2500)
    $log = Join-Path $GameDir 'bin\x64\Sunrise\logs\sunrise.log'
    $before = (Get-Content $log).Count
    [System.IO.File]::WriteAllText((Join-Path $GameDir 'SVR_Weapon.txt'), (($Lines -join "`n") + "`n"), (New-Object System.Text.UTF8Encoding($false)))
    Start-Sleep -Milliseconds $WaitMs
    Get-Content $log | Select-Object -Skip $before | Select-String 'ev=vr.weapon' | ForEach-Object { $_.Line -replace 'client level=warn ', '' }
}
