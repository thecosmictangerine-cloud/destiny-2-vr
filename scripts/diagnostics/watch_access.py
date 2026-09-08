"""
Who touches this address? A minimal Win32 debugger built on ctypes.

Attaches to destiny2.exe, arms one hardware data breakpoint (DR0) on every thread, collects the
instruction pointers that trip it for a few seconds, detaches, and disassembles each site from the
executable on disk with capstone so the game never has to be rebuilt or restarted.

    python watch_access.py 0x1F583265DCC --seconds 4 --len 4 --mode rw
    --mode w  : writes only          --mode rw : reads and writes (x86 has no read-only trap)

Reports "module+offset  count  threads" per site plus the instruction that did the access (the
trap fires after the instruction, so the reported RIP is the one following it).
"""
import argparse
import ctypes
import os
import struct
import sys
import time
from ctypes import wintypes

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)

# ---- constants ----------------------------------------------------------------------------
DBG_CONTINUE = 0x00010002
DBG_EXCEPTION_NOT_HANDLED = 0x80010001
EXCEPTION_DEBUG_EVENT = 1
CREATE_THREAD_DEBUG_EVENT = 2
CREATE_PROCESS_DEBUG_EVENT = 3
EXIT_THREAD_DEBUG_EVENT = 4
EXIT_PROCESS_DEBUG_EVENT = 5
LOAD_DLL_DEBUG_EVENT = 6
EXCEPTION_BREAKPOINT = 0x80000003
EXCEPTION_SINGLE_STEP = 0x80000004
CONTEXT_AMD64 = 0x00100000
CONTEXT_CONTROL = CONTEXT_AMD64 | 0x1
CONTEXT_INTEGER = CONTEXT_AMD64 | 0x2
CONTEXT_DEBUG_REGISTERS = CONTEXT_AMD64 | 0x10
TH32CS_SNAPPROCESS = 0x2
INFINITE = 0xFFFFFFFF


# ---- structures ---------------------------------------------------------------------------
class M128A(ctypes.Structure):
    _fields_ = [("Low", ctypes.c_ulonglong), ("High", ctypes.c_longlong)]


class CONTEXT(ctypes.Structure):
    _pack_ = 16
    _fields_ = [
        ("P1Home", ctypes.c_ulonglong), ("P2Home", ctypes.c_ulonglong), ("P3Home", ctypes.c_ulonglong),
        ("P4Home", ctypes.c_ulonglong), ("P5Home", ctypes.c_ulonglong), ("P6Home", ctypes.c_ulonglong),
        ("ContextFlags", wintypes.DWORD), ("MxCsr", wintypes.DWORD),
        ("SegCs", wintypes.WORD), ("SegDs", wintypes.WORD), ("SegEs", wintypes.WORD),
        ("SegFs", wintypes.WORD), ("SegGs", wintypes.WORD), ("SegSs", wintypes.WORD),
        ("EFlags", wintypes.DWORD),
        ("Dr0", ctypes.c_ulonglong), ("Dr1", ctypes.c_ulonglong), ("Dr2", ctypes.c_ulonglong),
        ("Dr3", ctypes.c_ulonglong), ("Dr6", ctypes.c_ulonglong), ("Dr7", ctypes.c_ulonglong),
        ("Rax", ctypes.c_ulonglong), ("Rcx", ctypes.c_ulonglong), ("Rdx", ctypes.c_ulonglong),
        ("Rbx", ctypes.c_ulonglong), ("Rsp", ctypes.c_ulonglong), ("Rbp", ctypes.c_ulonglong),
        ("Rsi", ctypes.c_ulonglong), ("Rdi", ctypes.c_ulonglong), ("R8", ctypes.c_ulonglong),
        ("R9", ctypes.c_ulonglong), ("R10", ctypes.c_ulonglong), ("R11", ctypes.c_ulonglong),
        ("R12", ctypes.c_ulonglong), ("R13", ctypes.c_ulonglong), ("R14", ctypes.c_ulonglong),
        ("R15", ctypes.c_ulonglong), ("Rip", ctypes.c_ulonglong),
        ("FltSave", ctypes.c_byte * 512),
        ("VectorRegister", M128A * 26), ("VectorControl", ctypes.c_ulonglong),
        ("DebugControl", ctypes.c_ulonglong), ("LastBranchToRip", ctypes.c_ulonglong),
        ("LastBranchFromRip", ctypes.c_ulonglong), ("LastExceptionToRip", ctypes.c_ulonglong),
        ("LastExceptionFromRip", ctypes.c_ulonglong),
    ]


class EXCEPTION_RECORD(ctypes.Structure):
    _fields_ = [
        ("ExceptionCode", wintypes.DWORD), ("ExceptionFlags", wintypes.DWORD),
        ("ExceptionRecord", ctypes.c_void_p), ("ExceptionAddress", ctypes.c_void_p),
        ("NumberParameters", wintypes.DWORD), ("ExceptionInformation", ctypes.c_ulonglong * 15),
    ]


class EXCEPTION_DEBUG_INFO(ctypes.Structure):
    _fields_ = [("ExceptionRecord", EXCEPTION_RECORD), ("dwFirstChance", wintypes.DWORD)]


class CREATE_THREAD_DEBUG_INFO(ctypes.Structure):
    _fields_ = [("hThread", wintypes.HANDLE), ("lpThreadLocalBase", ctypes.c_void_p),
                ("lpStartAddress", ctypes.c_void_p)]


class CREATE_PROCESS_DEBUG_INFO(ctypes.Structure):
    _fields_ = [("hFile", wintypes.HANDLE), ("hProcess", wintypes.HANDLE), ("hThread", wintypes.HANDLE),
                ("lpBaseOfImage", ctypes.c_void_p), ("dwDebugInfoFileOffset", wintypes.DWORD),
                ("nDebugInfoSize", wintypes.DWORD), ("lpThreadLocalBase", ctypes.c_void_p),
                ("lpStartAddress", ctypes.c_void_p), ("lpImageName", ctypes.c_void_p),
                ("fUnicode", wintypes.WORD)]


class LOAD_DLL_DEBUG_INFO(ctypes.Structure):
    _fields_ = [("hFile", wintypes.HANDLE), ("lpBaseOfDll", ctypes.c_void_p),
                ("dwDebugInfoFileOffset", wintypes.DWORD), ("nDebugInfoSize", wintypes.DWORD),
                ("lpImageName", ctypes.c_void_p), ("fUnicode", wintypes.WORD)]


class DEBUG_EVENT_UNION(ctypes.Union):
    _fields_ = [("Exception", EXCEPTION_DEBUG_INFO), ("CreateThread", CREATE_THREAD_DEBUG_INFO),
                ("CreateProcessInfo", CREATE_PROCESS_DEBUG_INFO), ("LoadDll", LOAD_DLL_DEBUG_INFO),
                ("raw", ctypes.c_byte * 160)]


class DEBUG_EVENT(ctypes.Structure):
    _fields_ = [("dwDebugEventCode", wintypes.DWORD), ("dwProcessId", wintypes.DWORD),
                ("dwThreadId", wintypes.DWORD), ("u", DEBUG_EVENT_UNION)]


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [("dwSize", wintypes.DWORD), ("cntUsage", wintypes.DWORD), ("th32ProcessID", wintypes.DWORD),
                ("th32DefaultHeapID", ctypes.c_void_p), ("th32ModuleID", wintypes.DWORD),
                ("cntThreads", wintypes.DWORD), ("th32ParentProcessID", wintypes.DWORD),
                ("pcPriClassBase", ctypes.c_long), ("dwFlags", wintypes.DWORD),
                ("szExeFile", ctypes.c_wchar * 260)]


class MODULEINFO(ctypes.Structure):
    _fields_ = [("lpBaseOfDll", ctypes.c_void_p), ("SizeOfImage", wintypes.DWORD), ("EntryPoint", ctypes.c_void_p)]


k32.DebugActiveProcess.argtypes = [wintypes.DWORD]
k32.DebugActiveProcessStop.argtypes = [wintypes.DWORD]
k32.DebugSetProcessKillOnExit.argtypes = [wintypes.BOOL]
k32.WaitForDebugEvent.argtypes = [ctypes.POINTER(DEBUG_EVENT), wintypes.DWORD]
k32.ContinueDebugEvent.argtypes = [wintypes.DWORD, wintypes.DWORD, wintypes.DWORD]
k32.GetThreadContext.argtypes = [wintypes.HANDLE, ctypes.POINTER(CONTEXT)]
k32.SetThreadContext.argtypes = [wintypes.HANDLE, ctypes.POINTER(CONTEXT)]
k32.OpenProcess.restype = wintypes.HANDLE
k32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
k32.ReadProcessMemory.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                  ctypes.POINTER(ctypes.c_size_t)]
psapi.EnumProcessModulesEx.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.HMODULE), wintypes.DWORD,
                                       ctypes.POINTER(wintypes.DWORD), wintypes.DWORD]
psapi.GetModuleFileNameExW.argtypes = [wintypes.HANDLE, wintypes.HMODULE, wintypes.LPWSTR, wintypes.DWORD]
psapi.GetModuleInformation.argtypes = [wintypes.HANDLE, wintypes.HMODULE, ctypes.POINTER(MODULEINFO), wintypes.DWORD]


def find_pid(name="destiny2.exe"):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    entry = PROCESSENTRY32W()
    entry.dwSize = ctypes.sizeof(entry)
    pid = None
    if k32.Process32FirstW(snap, ctypes.byref(entry)):
        while True:
            if entry.szExeFile.lower() == name:
                pid = entry.th32ProcessID
                break
            if not k32.Process32NextW(snap, ctypes.byref(entry)):
                break
    k32.CloseHandle(snap)
    return pid


def modules(hproc):
    """[(base, size, path)] of every module in the process."""
    count = wintypes.DWORD(0)
    psapi.EnumProcessModulesEx(hproc, None, 0, ctypes.byref(count), 0x03)
    n = count.value // ctypes.sizeof(wintypes.HMODULE)
    arr = (wintypes.HMODULE * n)()
    psapi.EnumProcessModulesEx(hproc, arr, ctypes.sizeof(arr), ctypes.byref(count), 0x03)
    out = []
    for h in arr:
        if not h:
            continue
        buf = ctypes.create_unicode_buffer(1024)
        psapi.GetModuleFileNameExW(hproc, h, buf, 1024)
        info = MODULEINFO()
        psapi.GetModuleInformation(hproc, h, ctypes.byref(info), ctypes.sizeof(info))
        out.append((info.lpBaseOfDll or 0, info.SizeOfImage, buf.value))
    return out


def read_mem(hproc, addr, size):
    buf = ctypes.create_string_buffer(size)
    got = ctypes.c_size_t(0)
    if not k32.ReadProcessMemory(hproc, ctypes.c_void_p(addr), buf, size, ctypes.byref(got)):
        return b""
    return buf.raw[: got.value]


def aligned_context():
    """CONTEXT must sit on a 16-byte boundary or Get/SetThreadContext fail with ERROR_NOACCESS."""
    buf = ctypes.create_string_buffer(ctypes.sizeof(CONTEXT) + 16)
    base = ctypes.addressof(buf)
    off = (16 - base % 16) % 16
    ctx = CONTEXT.from_buffer(buf, off)
    ctx._keep = buf
    return ctx


def set_dr0(hthread, addr, length, mode):
    ctx = aligned_context()
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS
    if not k32.GetThreadContext(hthread, ctypes.byref(ctx)):
        return False
    rw = 0b01 if mode == "w" else 0b11
    ln = {1: 0b00, 2: 0b01, 4: 0b11, 8: 0b10}[length]
    ctx.Dr0 = addr
    ctx.Dr7 = (ctx.Dr7 & ~0xF0003) | 0x1 | (rw << 16) | (ln << 18)
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS
    return bool(k32.SetThreadContext(hthread, ctypes.byref(ctx)))


def clear_dr0(hthread):
    ctx = aligned_context()
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS
    if k32.GetThreadContext(hthread, ctypes.byref(ctx)):
        ctx.Dr0 = 0
        ctx.Dr7 &= ~0xF0003
        ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS
        k32.SetThreadContext(hthread, ctypes.byref(ctx))


def disasm_site(code_bytes, base_va, rip, before=3, after=2):
    """Lines around rip. Finds an instruction boundary that lands exactly on rip."""
    try:
        from capstone import Cs, CS_ARCH_X86, CS_MODE_64
    except ImportError:
        return ["  (capstone not installed)"]
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    best = None
    for back in range(1, 48):
        start = rip - back
        if start < base_va:
            break
        insns = list(md.disasm(code_bytes[start - base_va:start - base_va + back + 64], start))
        addrs = [i.address for i in insns]
        if rip in addrs:
            idx = addrs.index(rip)
            best = insns[max(0, idx - before): idx + after + 1]
            if idx >= before:
                break
    if not best:
        return ["  (no boundary found)"]
    lines = []
    for i in best:
        mark = "->" if i.address == rip else "  "
        lines.append(f"  {mark} {i.address:#x}: {i.mnemonic} {i.op_str}")
    return lines


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("address", help="hex address to watch")
    ap.add_argument("--seconds", type=float, default=3.0)
    ap.add_argument("--len", type=int, default=4, choices=[1, 2, 4, 8])
    ap.add_argument("--mode", default="rw", choices=["w", "rw"])
    ap.add_argument("--max-hits", type=int, default=20000)
    ap.add_argument("--regs", action="store_true", help="dump registers on the first hit of each site")
    args = ap.parse_args()
    addr = int(args.address, 16)

    pid = find_pid()
    if not pid:
        sys.exit("destiny2.exe not running")
    hproc = k32.OpenProcess(0x1FFFFF, False, pid)
    mods = modules(hproc)
    k32.DebugSetProcessKillOnExit(False)
    if not k32.DebugActiveProcess(pid):
        sys.exit(f"DebugActiveProcess failed: {ctypes.get_last_error()}")

    threads = {}
    armed_ok = 0
    sites = {}
    regs_seen = {}
    hits = 0
    started = time.time()
    ev = DEBUG_EVENT()
    armed = False
    try:
        while True:
            elapsed = time.time() - started
            if elapsed > args.seconds or hits >= args.max_hits:
                break
            if not k32.WaitForDebugEvent(ctypes.byref(ev), 200):
                continue
            status = DBG_CONTINUE
            code = ev.dwDebugEventCode
            if code == CREATE_PROCESS_DEBUG_EVENT:
                threads[ev.dwThreadId] = ev.u.CreateProcessInfo.hThread
                armed_ok += int(set_dr0(ev.u.CreateProcessInfo.hThread, addr, args.len, args.mode))
            elif code == CREATE_THREAD_DEBUG_EVENT:
                threads[ev.dwThreadId] = ev.u.CreateThread.hThread
                armed_ok += int(set_dr0(ev.u.CreateThread.hThread, addr, args.len, args.mode))
                armed = True
            elif code == EXIT_THREAD_DEBUG_EVENT:
                threads.pop(ev.dwThreadId, None)
            elif code == EXIT_PROCESS_DEBUG_EVENT:
                print("process exited")
                break
            elif code == EXCEPTION_DEBUG_EVENT:
                rec = ev.u.Exception.ExceptionRecord
                if rec.ExceptionCode == EXCEPTION_SINGLE_STEP:
                    hthread = threads.get(ev.dwThreadId)
                    rip = rec.ExceptionAddress or 0
                    key = rip
                    entry = sites.setdefault(key, {"count": 0, "threads": set()})
                    entry["count"] += 1
                    entry["threads"].add(ev.dwThreadId)
                    hits += 1
                    if args.regs and key not in regs_seen and hthread:
                        ctx = aligned_context()
                        ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER
                        if k32.GetThreadContext(hthread, ctypes.byref(ctx)):
                            regs_seen[key] = {n: getattr(ctx, n) for n in
                                              ("Rax", "Rcx", "Rdx", "Rbx", "Rsp", "Rbp", "Rsi", "Rdi",
                                               "R8", "R9", "R10", "R11", "R12", "R13", "R14", "R15")}
                    status = DBG_CONTINUE
                elif rec.ExceptionCode == EXCEPTION_BREAKPOINT:
                    status = DBG_CONTINUE
                else:
                    status = DBG_EXCEPTION_NOT_HANDLED
            k32.ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, status)
    finally:
        for h in threads.values():
            clear_dr0(h)
        k32.DebugActiveProcessStop(pid)

    print(f"watched {addr:#x} len={args.len} mode={args.mode} for {time.time() - started:.1f}s: "
          f"{hits} hits at {len(sites)} sites, {len(threads)} threads, {armed_ok} armed ok (sizeof CONTEXT={ctypes.sizeof(CONTEXT):#x})")

    # Resolve sites to module+offset and disassemble from disk.
    file_cache = {}
    for rip, entry in sorted(sites.items(), key=lambda kv: -kv[1]["count"]):
        mod = next(((b, s, p) for (b, s, p) in mods if b <= rip < b + s), None)
        if mod:
            base, size, path = mod
            name = os.path.basename(path)
            print(f"\n{name}+{rip - base:#x}  count={entry['count']}  threads={sorted(entry['threads'])}")
            # Read code from the live process (already relocated) rather than from disk.
            lo = max(base, rip - 64)
            code = read_mem(hproc, lo, 128)
            if code:
                for line in disasm_site(code, lo, rip):
                    print(line)
        else:
            print(f"\n{rip:#x} (no module)  count={entry['count']}")
        if rip in regs_seen:
            r = regs_seen[rip]
            print("   " + " ".join(f"{k}={v:#x}" for k, v in r.items()))


if __name__ == "__main__":
    main()
