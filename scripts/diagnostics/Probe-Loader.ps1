# Compares loader entry points in the running game against this process, and reads the game's
# process mitigation policies. ntdll/kernelbase are mapped at the same base in every process for a
# boot, so a byte difference at an export means the game (or something in it) patched it.
$ErrorActionPreference = 'Stop'
Add-Type -Namespace P -Name N -MemberDefinition @'
[DllImport("kernel32.dll", SetLastError=true)] public static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
[DllImport("kernel32.dll", SetLastError=true)] public static extern bool ReadProcessMemory(IntPtr h, IntPtr addr, byte[] buf, int size, out int read);
[DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] public static extern IntPtr GetModuleHandleW(string name);
[DllImport("kernel32.dll", CharSet=CharSet.Ansi, SetLastError=true)] public static extern IntPtr GetProcAddress(IntPtr m, string name);
[DllImport("kernel32.dll", SetLastError=true)] public static extern bool GetProcessMitigationPolicy(IntPtr h, int policy, out ulong buf, IntPtr size);
[DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
'@
$p = Get-Process destiny2 -ErrorAction Stop | Select-Object -First 1
$h = [P.N]::OpenProcess(0x0410, $false, $p.Id)  # QUERY_INFORMATION | VM_READ
if ($h -eq [IntPtr]::Zero) { throw "OpenProcess failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())" }

$targets = @(
    @('ntdll.dll', 'LdrLoadDll'), @('ntdll.dll', 'LdrpLoadDll'), @('ntdll.dll', 'NtMapViewOfSection'),
    @('ntdll.dll', 'NtCreateSection'), @('ntdll.dll', 'NtOpenFile'), @('ntdll.dll', 'NtProtectVirtualMemory'),
    @('ntdll.dll', 'LdrGetProcedureAddress'), @('ntdll.dll', 'NtQueryAttributesFile'),
    @('kernelbase.dll', 'LoadLibraryExW'), @('kernel32.dll', 'LoadLibraryW'), @('kernel32.dll', 'LoadLibraryExW'),
    @('kernelbase.dll', 'GetProcAddress'), @('kernel32.dll', 'FlsAlloc'), @('kernelbase.dll', 'FlsAlloc')
)
foreach ($t in $targets) {
    $m = [P.N]::GetModuleHandleW($t[0])
    $a = [P.N]::GetProcAddress($m, $t[1])
    if ($a -eq [IntPtr]::Zero) { Write-Output ('{0}!{1}: not found locally' -f $t); continue }
    $mine = New-Object byte[] 16
    [Runtime.InteropServices.Marshal]::Copy($a, $mine, 0, 16)
    $theirs = New-Object byte[] 16
    $read = 0
    $ok = [P.N]::ReadProcessMemory($h, $a, $theirs, 16, [ref]$read)
    $hexM = ($mine | ForEach-Object { $_.ToString('X2') }) -join ' '
    $hexT = ($theirs | ForEach-Object { $_.ToString('X2') }) -join ' '
    $same = if ($ok -and $hexM -eq $hexT) { 'SAME' } elseif (-not $ok) { 'READ FAIL ' + [Runtime.InteropServices.Marshal]::GetLastWin32Error() } else { 'DIFFERENT' }
    Write-Output ('{0,-38} {1}  mine={2}' -f ("$($t[0])!$($t[1])"), $same, $hexM)
    if ($same -eq 'DIFFERENT') { Write-Output ('{0,-38} theirs={1}' -f '', $hexT) }
}
# Mitigation policies: 0 DEP, 1 ASLR, 2 DynamicCode, 3 StrictHandle, 4 SystemCallDisable, 5 MitigationOptionsMask,
# 6 ExtensionPointDisable, 7 ControlFlowGuard, 8 Signature, 9 FontDisable, 10 ImageLoad, 11 SideChannel, 12 UserShadowStack
$names = @{0='DEP';1='ASLR';2='DynamicCode';3='StrictHandle';4='SystemCallDisable';6='ExtensionPointDisable';7='CFG';8='Signature';9='FontDisable';10='ImageLoad';11='SideChannel';12='UserShadowStack'}
foreach ($k in ($names.Keys | Sort-Object)) {
    $v = [uint64]0
    $ok = [P.N]::GetProcessMitigationPolicy($h, $k, [ref]$v, [IntPtr]8)
    Write-Output ('policy {0,-22} ok={1} flags=0x{2:X}' -f $names[$k], $ok, $v)
}
[void][P.N]::CloseHandle($h)
# Modules loaded in the game that are not from Windows or the game dir
Write-Output '---- foreign modules in game'
$p.Modules | Where-Object { $_.FileName -notmatch '^C:\\Windows\\' -and $_.FileName -notmatch '^C:\\Games\\Sunrise\\' } | ForEach-Object { $_.FileName }
Write-Output ('---- module count {0}' -f $p.Modules.Count)
