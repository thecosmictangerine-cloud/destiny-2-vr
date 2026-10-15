"""
Static analysis of destiny2.exe as it sits DECRYPTED in the running process.

VMProtect encrypts the first .text on disk, so every disassembly has to start from the live image
(RESEARCH.md, "VMProtect"). This snapshots the mapped image once into a scratch file, outside the
repo -- it is Bungie code and must never be committed -- and then answers questions about it
offline, without touching the process again:

    python live_image.py dump [--out PATH]          read every section of the live image
    python live_image.py find "8D 14 33 48 8B CF E8 ? ? ? ?"
    python live_image.py func +0xD5D832             the function containing an RVA (.pdata)
    python live_image.py dis +0xD5D7F0 [0x200]      disassemble from the snapshot
    python live_image.py fdis +0xD5D832             disassemble the whole containing function
    python live_image.py xref +0x12D22C0            near calls/jumps to an RVA, and rip-relative refs
    python live_image.py calls +0xD5D7F0            every near call a function makes, with targets

Only reads: ReadProcessMemory against the live process. Nothing here raises an exception in the
game, which is the one thing VMProtect punishes.
"""
import ctypes
import os
import re
import struct
import subprocess
import sys
from ctypes import wintypes

from capstone import CS_ARCH_X86, CS_MODE_64, Cs

DEFAULT = os.path.join(os.environ.get("TEMP", "."), "claude", "d2_live_image.bin")

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
        return None
    return buf.raw[: got.value]


def read_paged(hproc, addr, size):
    """Reads page by page, zero-filling anything unreadable, so one hole does not lose a section."""
    out = bytearray(size)
    page = 0x1000
    off = 0
    while off < size:
        n = min(0x100000, size - off)
        chunk = read(hproc, addr + off, n)
        if chunk is not None and len(chunk) == n:
            out[off:off + n] = chunk
        else:
            for p in range(off, off + n, page):
                m = min(page, size - p)
                c = read(hproc, addr + p, m)
                if c:
                    out[p:p + len(c)] = c
        off += n
    return bytes(out)


def cmd_dump(out_path):
    pid = pid_of()
    if not pid:
        sys.exit("destiny2.exe not running")
    hproc = k32.OpenProcess(0x0410, False, pid)
    base = exe_base(hproc)
    head = read(hproc, base, 0x1000)
    e_lfanew = struct.unpack_from("<I", head, 0x3C)[0]
    nsec = struct.unpack_from("<H", head, e_lfanew + 6)[0]
    optsize = struct.unpack_from("<H", head, e_lfanew + 20)[0]
    size_of_image = struct.unpack_from("<I", head, e_lfanew + 24 + 56)[0]
    sec0 = e_lfanew + 24 + optsize
    img = bytearray(size_of_image)
    img[:0x1000] = head
    sections = []
    for i in range(nsec):
        o = sec0 + 40 * i
        name = head[o:o + 8].rstrip(b"\0").decode(errors="replace")
        vsize, vaddr = struct.unpack_from("<II", head, o + 8)
        chars = struct.unpack_from("<I", head, o + 36)[0]
        data = read_paged(hproc, base + vaddr, vsize)
        img[vaddr:vaddr + vsize] = data
        sections.append((name, vaddr, vsize, chars))
        print(f"  {name:8} rva={vaddr:#010x} size={vsize:#010x} chars={chars:#010x}")
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "wb") as f:
        f.write(img)
    with open(out_path + ".base", "w") as f:
        f.write(f"{base:#x}\n")
    print(f"dumped {len(img):#x} bytes from base {base:#x} to {out_path}")


class Image:
    def __init__(self, path=DEFAULT):
        with open(path, "rb") as f:
            self.data = f.read()
        head = self.data
        e = struct.unpack_from("<I", head, 0x3C)[0]
        nsec = struct.unpack_from("<H", head, e + 6)[0]
        optsize = struct.unpack_from("<H", head, e + 20)[0]
        sec0 = e + 24 + optsize
        self.sections = []
        for i in range(nsec):
            o = sec0 + 40 * i
            name = head[o:o + 8].rstrip(b"\0").decode(errors="replace")
            vsize, vaddr = struct.unpack_from("<II", head, o + 8)
            chars = struct.unpack_from("<I", head, o + 36)[0]
            self.sections.append((name, vaddr, vsize, chars))
        # Data directory 3 = exception (.pdata): RUNTIME_FUNCTION { begin, end, unwind } rvas.
        dd = e + 24 + 112
        exc_rva, exc_size = struct.unpack_from("<II", head, dd + 3 * 8)
        self.funcs = []
        for k in range(exc_size // 12):
            b, en, u = struct.unpack_from("<III", self.data, exc_rva + 12 * k)
            if b and en > b:
                self.funcs.append((b, en, u))
        self.funcs.sort()
        self.md = Cs(CS_ARCH_X86, CS_MODE_64)
        self.md.detail = False

    def code_sections(self):
        return [s for s in self.sections if s[3] & 0x20000000]

    def func_of(self, rva):
        import bisect
        i = bisect.bisect_right(self.funcs, (rva, 0xFFFFFFFF, 0xFFFFFFFF)) - 1
        while i >= 0:
            b, e, u = self.funcs[i]
            if b <= rva < e:
                # Chained unwind entries describe fragments of one function; walk back to the primary.
                return self.primary(b, e, u)
            if e <= rva and rva - e > 0x10000:
                break
            i -= 1
        return None

    def primary(self, b, e, u):
        # UNWIND_INFO: flags in the top 5 bits of byte 0; UNW_FLAG_CHAININFO = 4.
        seen = 0
        while seen < 16:
            ver_flags = self.data[u]
            flags = ver_flags >> 3
            if not flags & 4:
                return (b, e)
            count = self.data[u + 2]
            off = u + 4 + ((count + 1) & ~1) * 2
            b2, e2, u2 = struct.unpack_from("<III", self.data, off)
            b, e, u = b2, e2, u2
            seen += 1
        return (b, e)

    def dis(self, rva, size):
        return list(self.md.disasm(self.data[rva:rva + size], rva))

    def find(self, pattern):
        toks = pattern.split()
        rx = b"".join(b"." if t in ("?", "??") else re.escape(bytes([int(t, 16)])) for t in toks)
        rx = re.compile(rx, re.S)
        hits = []
        for name, va, vs, ch in self.code_sections():
            for m in rx.finditer(self.data, va, va + vs):
                hits.append(m.start())
        return hits

    def xref(self, target):
        hits = []
        for name, va, vs, ch in self.code_sections():
            d = self.data
            # E8/E9 rel32
            for op in (0xE8, 0xE9):
                start = va
                b = bytes([op])
                while True:
                    i = d.find(b, start, va + vs - 4)
                    if i < 0:
                        break
                    rel = struct.unpack_from("<i", d, i + 1)[0]
                    if i + 5 + rel == target:
                        hits.append((i, "call" if op == 0xE8 else "jmp"))
                    start = i + 1
        return hits

    def riprefs(self, target, window=None):
        """rip-relative disp32 references to target (any instruction): brute force on disp32 positions."""
        hits = []
        d = self.data
        for name, va, vs, ch in self.code_sections():
            for i in range(va, va + vs - 4):
                rel = struct.unpack_from("<i", d, i)[0]
                # disp32 is followed by 0..4 bytes of immediate; the common case is end-of-insn.
                for tail in (4, 5, 8):
                    if i + tail + rel == target:
                        hits.append(i)
        return hits


def parse_rva(s):
    s = s.strip()
    if s.startswith("+"):
        return int(s[1:], 16)
    return int(s, 16)


def fmt(img, insns):
    for i in insns:
        print(f"  +{i.address:#x}: {i.bytes.hex():24} {i.mnemonic} {i.op_str}")


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return
    cmd = args[0]
    if cmd == "dump":
        out = args[2] if len(args) > 2 and args[1] == "--out" else DEFAULT
        cmd_dump(out)
        return
    img = Image()
    if cmd == "find":
        for h in img.find(args[1]):
            f = img.func_of(h)
            print(f"  +{h:#x}  in func {('+%#x..+%#x' % f) if f else '?'}")
    elif cmd == "func":
        r = parse_rva(args[1])
        f = img.func_of(r)
        print(f"  +{r:#x} in +{f[0]:#x}..+{f[1]:#x} ({f[1] - f[0]:#x} bytes)" if f else "  no .pdata entry")
    elif cmd == "dis":
        r = parse_rva(args[1])
        n = int(args[2], 16) if len(args) > 2 else 0x100
        fmt(img, img.dis(r, n))
    elif cmd == "fdis":
        r = parse_rva(args[1])
        f = img.func_of(r)
        if not f:
            sys.exit("no .pdata entry")
        print(f"--- function +{f[0]:#x}..+{f[1]:#x}")
        fmt(img, img.dis(f[0], f[1] - f[0]))
    elif cmd == "xref":
        r = parse_rva(args[1])
        for h, kind in img.xref(r):
            f = img.func_of(h)
            print(f"  {kind} at +{h:#x}  in func {('+%#x' % f[0]) if f else '?'}")
    elif cmd == "riprefs":
        r = parse_rva(args[1])
        for h in img.riprefs(r):
            f = img.func_of(h)
            print(f"  disp at +{h:#x}  in func {('+%#x' % f[0]) if f else '?'}")
    elif cmd == "calls":
        r = parse_rva(args[1])
        f = img.func_of(r)
        if not f:
            sys.exit("no .pdata entry")
        for i in img.dis(f[0], f[1] - f[0]):
            if i.mnemonic in ("call", "jmp") and i.op_str.startswith("0x"):
                t = int(i.op_str, 16)
                print(f"  +{i.address:#x}: {i.mnemonic} +{t:#x}")
            elif i.mnemonic == "call":
                print(f"  +{i.address:#x}: call {i.op_str}")
    else:
        print(__doc__)


if __name__ == "__main__":
    main()
