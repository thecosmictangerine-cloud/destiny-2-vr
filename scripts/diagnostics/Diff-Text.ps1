# Diffs the code pages of system modules between the running game and this process. Both map the
# same image at the same base for a boot, so any difference is a patch made inside the game.
$ErrorActionPreference = 'Stop'
Add-Type -Namespace P4 -Name N -MemberDefinition @'
[DllImport("kernel32.dll", SetLastError=true)] public static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
[DllImport("kernel32.dll", SetLastError=true)] public static extern bool ReadProcessMemory(IntPtr h, IntPtr addr, byte[] buf, int size, out int read);
[DllImport("kernel32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr GetModuleHandleW(string name);
[DllImport("kernel32.dll", CharSet=CharSet.Ansi)] public static extern IntPtr GetProcAddress(IntPtr m, string name);
[DllImport("dbghelp.dll", CharSet=CharSet.Unicode, SetLastError=true)] public static extern IntPtr ImageNtHeader(IntPtr imageBase);
'@
$p = Get-Process destiny2 | Select-Object -First 1
$h = [P4.N]::OpenProcess(0x0410, $false, $p.Id)
foreach ($modName in @('ntdll.dll', 'kernelbase.dll', 'kernel32.dll')) {
    $base = [P4.N]::GetModuleHandleW($modName)
    $nt = [P4.N]::ImageNtHeader($base)
    # IMAGE_NT_HEADERS64: Signature(4) FileHeader(20) OptionalHeader(240) then sections
    $numSections = [Runtime.InteropServices.Marshal]::ReadInt16($nt, 6)
    $optSize = [Runtime.InteropServices.Marshal]::ReadInt16($nt, 20)
    $sec = [IntPtr]($nt.ToInt64() + 24 + $optSize)
    for ($i = 0; $i -lt $numSections; $i++) {
        $s = [IntPtr]($sec.ToInt64() + $i * 40)
        $nameBytes = New-Object byte[] 8; [Runtime.InteropServices.Marshal]::Copy($s, $nameBytes, 0, 8)
        $name = [Text.Encoding]::ASCII.GetString($nameBytes).TrimEnd([char]0)
        $vsize = [Runtime.InteropServices.Marshal]::ReadInt32($s, 8)
        $vaddr = [Runtime.InteropServices.Marshal]::ReadInt32($s, 12)
        $chars = [Runtime.InteropServices.Marshal]::ReadInt32($s, 36)
        if (($chars -band 0x20000000) -eq 0) { continue }  # IMAGE_SCN_MEM_EXECUTE
        $start = $base.ToInt64() + $vaddr
        $mine = New-Object byte[] $vsize
        [Runtime.InteropServices.Marshal]::Copy([IntPtr]$start, $mine, 0, $vsize)
        $theirs = New-Object byte[] $vsize
        $read = 0
        $ok = [P4.N]::ReadProcessMemory($h, [IntPtr]$start, $theirs, $vsize, [ref]$read)
        Write-Output ('{0} {1} size=0x{2:X} read={3}' -f $modName, $name, $vsize, $ok)
        $diffs = 0; $runStart = -1
        for ($k = 0; $k -lt $vsize; $k++) {
            if ($mine[$k] -ne $theirs[$k]) { if ($runStart -lt 0) { $runStart = $k }; $diffs++ }
            elseif ($runStart -ge 0) {
                $addr = $start + $runStart
                $rva = $runStart + $vaddr
                $len = $k - $runStart
                $hexMine = ($mine[$runStart..($k-1)] | ForEach-Object { $_.ToString('X2') }) -join ' '
                $hexTheirs = ($theirs[$runStart..($k-1)] | ForEach-Object { $_.ToString('X2') }) -join ' '
                Write-Output ('  patch @0x{0:X} (+0x{1:X}) len={2} mine={3} theirs={4}' -f $addr, $rva, $len, $hexMine, $hexTheirs)
                $runStart = -1
            }
        }
        Write-Output ('  differing bytes: {0}' -f $diffs)
    }
}
# name the exports nearest to each patch in ntdll: dump export table RVAs
Write-Output '---- nearest exports (ntdll) are resolved below by the caller'
