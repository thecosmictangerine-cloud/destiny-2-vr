"""
Linear disassembly of a live code range in destiny2.exe.

    python disasm_range.py +0x12D2200 0x140          # RVA (leading +) and byte count
    python disasm_range.py 0x7FF758352200 0x140      # absolute address
    python disasm_range.py +0xB3598D -0x40           # 0x40 bytes ENDING at the RVA (call site view)

RVA form resolves the module base from the running process. A negative count disassembles the
bytes before the address, re-synchronising so the last instruction ends exactly at it.
"""
import ctypes
import subprocess
import sys
from ctypes import wintypes

from capstone import CS_ARCH_X86, CS_MODE_64, Cs

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)
k32.OpenProcess.restype = wintypes.HANDLE
k32.ReadProcessMemory.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                  ctypes.POINTER(ctypes.c_size_t)]
psapi.EnumProcessModulesEx.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.HMODULE), wintypes.DWORD,
                                       ctypes.POINTER(wintypes.DWORD), wintypes.DWORD]
psapi.GetModuleFileNameExW.argtypes = [wintypes.HANDLE, wintypes.HMODULE, wintypes.LPWSTR, wintypes.DWORD]


def pid_of(name="destiny2.exe"):
    out = subprocess.check_output(["tasklist", "/FI", f"IMAGENAME eq {name}", "/FO", "CSV"]).decode(errors="ignore")
    for line in out.splitlines()[1:]:
        parts = line.strip('"').split('","')
        if len(parts) > 1:
            return int(parts[1])
    return None


def exe_base(hproc):
    count = wintypes.DWORD(0)
    psapi.EnumProcessModulesEx(hproc, None, 0, ctypes.byref(count), 0x03)
    n = count.value // ctypes.sizeof(wintypes.HMODULE)
    arr = (wintypes.HMODULE * n)()
    psapi.EnumProcessModulesEx(hproc, arr, ctypes.sizeof(arr), ctypes.byref(count), 0x03)
    for h in arr:
        buf = ctypes.create_unicode_buffer(1024)
        psapi.GetModuleFileNameExW(hproc, h, buf, 1024)
        if buf.value.lower().endswith("destiny2.exe"):
            return h
    return None


def read(hproc, addr, size):
    buf = ctypes.create_string_buffer(size)
    got = ctypes.c_size_t(0)
    if not k32.ReadProcessMemory(hproc, ctypes.c_void_p(addr), buf, size, ctypes.byref(got)):
        return b""
    return buf.raw[: got.value]


def main():
    pid = pid_of()
    if not pid:
        sys.exit("destiny2.exe not running")
    hproc = k32.OpenProcess(0x0410, False, pid)
    base = exe_base(hproc)
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    args = sys.argv[1:]
    while args:
        spec, count = args[0], int(args[1], 16) if len(args) > 1 else 0x80
        args = args[2:]
        if spec.startswith("+"):
            addr = base + int(spec[1:], 16)
        else:
            addr = int(spec, 16)
        if count < 0:
            end = addr
            size = -count
            code = read(hproc, end - size, size + 16)
            best = None
            for back in range(size, size - 15, -1):
                start = end - back
                insns = list(md.disasm(code[start - (end - size):], start))
                if any(i.address + i.size == end for i in insns):
                    best = [i for i in insns if i.address < end]
                    break
            insns = best or []
            print(f"\n--- {size:#x} bytes ending at destiny2.exe+{end - base:#x}")
        else:
            code = read(hproc, addr, count)
            insns = list(md.disasm(code, addr))
            print(f"\n--- destiny2.exe+{addr - base:#x} ({count:#x} bytes)")
        for i in insns:
            print(f"  +{i.address - base:#x}: {i.mnemonic} {i.op_str}")


if __name__ == "__main__":
    main()
