"""
Static disassembly of destiny2.exe by RVA, straight from the file on disk.

    python disasm_file.py 0x12D2306 0x36B073 ...        # a few lines around each RVA
    python disasm_file.py 0x12D2306 --before 12 --after 6

RIP-relative operands are shown as capstone prints them (relative to the RVA), so an
`[rip + X]` target is RVA-based too. The trap-after-access convention of the watch applies: pass
the reported RVA and read the instruction ABOVE the `->` line for the memory access.
"""
import argparse
import struct
import sys

from capstone import CS_ARCH_X86, CS_MODE_64, Cs

EXE = r"C:\Games\Sunrise\destiny2.exe"


def sections(path):
    with open(path, "rb") as f:
        data = f.read(0x1000)
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    count = struct.unpack_from("<H", data, pe + 6)[0]
    opt_size = struct.unpack_from("<H", data, pe + 20)[0]
    first = pe + 24 + opt_size
    out = []
    for i in range(count):
        s = data[first + i * 40: first + (i + 1) * 40]
        name = s[:8].rstrip(b"\0").decode(errors="ignore")
        vsize, va, rawsize, raw = struct.unpack_from("<IIII", s, 8)
        out.append((name, va, vsize, raw, rawsize))
    return out


def rva_to_file(secs, rva):
    for name, va, vsize, raw, rawsize in secs:
        if va <= rva < va + max(vsize, rawsize):
            return raw + (rva - va), name
    return None, None


def read_rva(path, secs, rva, size):
    off, name = rva_to_file(secs, rva)
    if off is None:
        return b"", None
    with open(path, "rb") as f:
        f.seek(off)
        return f.read(size), name


def disasm_around(path, secs, rva, before, after):
    lo = rva - 128
    code, name = read_rva(path, secs, lo, 128 + 96)
    if not code:
        return [f"  (rva {rva:#x} not in any section)"]
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    best = None
    for back in range(1, 120):
        start = rva - back
        insns = list(md.disasm(code[start - lo:], start))
        addrs = [i.address for i in insns]
        if rva in addrs:
            idx = addrs.index(rva)
            cand = insns[max(0, idx - before): idx + after + 1]
            if best is None or len(cand) > len(best):
                best = cand
            if idx >= before:
                break
    if not best:
        return [f"  (no boundary found for {rva:#x} in {name})"]
    return [f"  {'->' if i.address == rva else '  '} {i.address:#x}: {i.mnemonic} {i.op_str}" for i in best]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("rvas", nargs="+")
    ap.add_argument("--before", type=int, default=8)
    ap.add_argument("--after", type=int, default=4)
    ap.add_argument("--exe", default=EXE)
    args = ap.parse_args()
    secs = sections(args.exe)
    for text in args.rvas:
        rva = int(text, 16)
        print(f"\ndestiny2.exe+{rva:#x}")
        for line in disasm_around(args.exe, secs, rva, args.before, args.after):
            print(line)


if __name__ == "__main__":
    main()
