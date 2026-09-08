# Names the patched RVAs: nearest export below each, then Microsoft symbols via dbghelp if reachable.
$ErrorActionPreference = 'Continue'
Add-Type -Namespace P5 -Name N -MemberDefinition @'
[DllImport("kernel32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr GetModuleHandleW(string name);
[DllImport("dbghelp.dll", SetLastError=true)] public static extern IntPtr ImageNtHeader(IntPtr imageBase);
[DllImport("dbghelp.dll", SetLastError=true)] public static extern bool SymInitialize(IntPtr hProcess, string UserSearchPath, bool fInvadeProcess);
[DllImport("dbghelp.dll", SetLastError=true)] public static extern bool SymCleanup(IntPtr hProcess);
[DllImport("dbghelp.dll", SetLastError=true)] public static extern uint SymSetOptions(uint SymOptions);
[DllImport("dbghelp.dll", SetLastError=true)] public static extern bool SymFromAddr(IntPtr hProcess, ulong Address, out ulong Displacement, IntPtr Symbol);
[DllImport("kernel32.dll")] public static extern IntPtr GetCurrentProcess();
'@
$patches = @(
    @('ntdll.dll', 0x11ECE0), @('ntdll.dll', 0x130DC1),
    @('kernelbase.dll', 0x213D0), @('kernelbase.dll', 0xC3AB1),
    @('kernel32.dll', 0x32060), @('kernel32.dll', 0x32EF0)
)
function Nearest-Export([IntPtr]$base, [int]$rva) {
    $nt = [P5.N]::ImageNtHeader($base)
    $exportRva = [Runtime.InteropServices.Marshal]::ReadInt32($nt, 24 + 112)  # DataDirectory[0].VirtualAddress
    $exp = [IntPtr]($base.ToInt64() + $exportRva)
    $numFuncs = [Runtime.InteropServices.Marshal]::ReadInt32($exp, 20)
    $numNames = [Runtime.InteropServices.Marshal]::ReadInt32($exp, 24)
    $funcs = [IntPtr]($base.ToInt64() + [Runtime.InteropServices.Marshal]::ReadInt32($exp, 28))
    $names = [IntPtr]($base.ToInt64() + [Runtime.InteropServices.Marshal]::ReadInt32($exp, 32))
    $ords = [IntPtr]($base.ToInt64() + [Runtime.InteropServices.Marshal]::ReadInt32($exp, 36))
    $best = $null; $bestRva = -1
    for ($i = 0; $i -lt $numNames; $i++) {
        $ord = [Runtime.InteropServices.Marshal]::ReadInt16($ords, $i * 2)
        $frva = [Runtime.InteropServices.Marshal]::ReadInt32($funcs, $ord * 4)
        if ($frva -le $rva -and $frva -gt $bestRva) {
            $bestRva = $frva
            $best = [Runtime.InteropServices.Marshal]::PtrToStringAnsi([IntPtr]($base.ToInt64() + [Runtime.InteropServices.Marshal]::ReadInt32($names, $i * 4)))
        }
    }
    return ('{0}+0x{1:X}' -f $best, ($rva - $bestRva))
}
$hp = [P5.N]::GetCurrentProcess()
[void][P5.N]::SymSetOptions(0x00000002 -bor 0x00000004 -bor 0x00000010)  # UNDNAME | DEFERRED_LOADS | LOAD_LINES
$symOk = [P5.N]::SymInitialize($hp, 'srv*C:\symbols*https://msdl.microsoft.com/download/symbols', $true)
Write-Output "SymInitialize=$symOk"
# SYMBOL_INFO: SizeOfStruct(4) TypeIndex(4) Reserved(16) Index(4) Size(4) ModBase(8) Flags(4) Value(8) Address(8) Register(4) Scope(4) Tag(4) NameLen(4) MaxNameLen(4) Name[1] => 88 bytes header
$buf = [Runtime.InteropServices.Marshal]::AllocHGlobal(88 + 1024)
foreach ($pt in $patches) {
    $base = [P5.N]::GetModuleHandleW($pt[0])
    $addr = [uint64]($base.ToInt64() + $pt[1])
    $near = Nearest-Export $base $pt[1]
    $sym = '?'
    if ($symOk) {
        for ($z = 0; $z -lt 88 + 1024; $z++) { [Runtime.InteropServices.Marshal]::WriteByte($buf, $z, 0) }
        [Runtime.InteropServices.Marshal]::WriteInt32($buf, 0, 88)
        [Runtime.InteropServices.Marshal]::WriteInt32($buf, 80, 1024)
        $disp = [uint64]0
        if ([P5.N]::SymFromAddr($hp, $addr, [ref]$disp, $buf)) {
            $sym = [Runtime.InteropServices.Marshal]::PtrToStringAnsi([IntPtr]($buf.ToInt64() + 84)) + ('+0x{0:X}' -f $disp)
        } else { $sym = 'symfail ' + [Runtime.InteropServices.Marshal]::GetLastWin32Error() }
    }
    Write-Output ('{0}+0x{1:X}  nearest-export={2}  symbol={3}' -f $pt[0], $pt[1], $near, $sym)
}
[Runtime.InteropServices.Marshal]::FreeHGlobal($buf)
if ($symOk) { [void][P5.N]::SymCleanup($hp) }
