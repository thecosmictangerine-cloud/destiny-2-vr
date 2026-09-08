"""
Disassembles the sites an access watch reported, reading code from the live game process.

    python disasm_sites.py                # every site in C:\Games\Sunrise\SVR_Watch.log (last watch)
    python disasm_sites.py 0x7FF6... ...  # explicit absolute addresses

The watch reports the instruction AFTER the access (hardware data breakpoints trap post-execution),
so the line marked `->` is that one and the line above it is the instruction that touched memory.
"""
import ctypes
import os
import re
import subprocess
import sys
from ctypes import wintypes

from capstone import CS_ARCH_X86, CS_MODE_64, Cs

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.OpenProcess.restype = wintypes.HANDLE
k32.ReadProcessMemory.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                  ctypes.POINTER(ctypes.c_size_t)]

REPORT = r"C:\Games\Sunrise\SVR_Watch.log"


def pid_of(name="destiny2.exe"):
    out = subprocess.check_output(["tasklist", "/FI", f"IMAGENAME eq {name}", "/FO", "CSV"]).decode(errors="ignore")
    for line in out.splitlines()[1:]:
        parts = line.strip('"').split('","')
        if len(parts) > 1:
            return int(parts[1])
    return None


def read(hproc, addr, size):
    buf = ctypes.create_string_buffer(size)
    got = ctypes.c_size_t(0)
    if not k32.ReadProcessMemory(hproc, ctypes.c_void_p(addr), buf, size, ctypes.byref(got)):
        return b""
    return buf.raw[: got.value]


def disasm(hproc, rip, before=6, after=3):
    lo = rip - 96
    code = read(hproc, lo, 96 + 64)
    if not code:
        return ["  (unreadable)"]
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    best = None
    for back in range(1, 90):
        start = rip - back
        insns = list(md.disasm(code[start - lo:], start))
        addrs = [i.address for i in insns]
        if rip in addrs:
            idx = addrs.index(rip)
            cand = insns[max(0, idx - before): idx + after + 1]
            if best is None or len(cand) > len(best):
                best = cand
            if idx >= before:
                break
    if not best:
        return ["  (no instruction boundary found)"]
    return [f"  {'->' if i.address == rip else '  '} {i.address:#x}: {i.mnemonic} {i.op_str}" for i in best]


def last_watch_sites(path):
    text = open(path, encoding="utf-8", errors="ignore").read()
    blocks = text.split("watch addr=")
    if len(blocks) < 2:
        return []
    last = "watch addr=" + blocks[-1]
    sites = []
    pattern = r"site rip=(\S+) abs=0x([0-9A-Fa-f]+) top=(\S+) count=(\d+) threads=(\S*)\n  regs (.*)(?:\n  stack(.*))?"
    for m in re.finditer(pattern, last):
        sites.append({"name": m.group(1), "abs": int(m.group(2), 16), "top": m.group(3),
                      "count": int(m.group(4)), "threads": m.group(5), "regs": m.group(6),
                      "stack": (m.group(7) or "").strip()})
    print(last.splitlines()[0])
    return sites


def main():
    pid = pid_of()
    if not pid:
        sys.exit("destiny2.exe not running")
    hproc = k32.OpenProcess(0x0410, False, pid)
    if len(sys.argv) > 1:
        sites = [{"name": a, "abs": int(a, 16), "top": "", "count": 0, "threads": "", "regs": ""} for a in sys.argv[1:]]
    else:
        sites = last_watch_sites(REPORT)
    for s in sorted(sites, key=lambda s: -s["count"]):
        print(f"\n{s['name']}  count={s['count']} threads={s['threads']} top={s['top']}")
        if s["regs"]:
            print("  regs " + s["regs"])
        if s.get("stack"):
            print("  stack " + s["stack"])
        for line in disasm(hproc, s["abs"]):
            print(line)


if __name__ == "__main__":
    main()
