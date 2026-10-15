"""Looks for the character's aim angles in memory: the value a shot is fired along.

Measured 2026-10-15 (docs/AIM.md): the shot follows the CHARACTER's aim -- the pitch/yaw the mouse
drives -- and none of the camera-pose paths the weapon module detours. If that aim is stored as a
plain angle (or a direction) somewhere, the module could write it directly: exact, with no injected
input and no servo lag.

Method: sample memory at two known aims and keep every float whose change matches the change of
aim, in any plausible encoding (radians, degrees, the sine or cosine, a direction component).

    python aim_scan.py block <addr> save <pitch>      sample the camera block (+0..+0x1000)
    python aim_scan.py block <addr> cmp <pitch>       compare against the saved sample
    python aim_scan.py heap save <pitch>              sample every private RW region (slow, ~GBs)
    python aim_scan.py heap cmp <pitch>               keep candidates whose change matches
    python aim_scan.py heap again <pitch>             narrow the candidate set with a third aim
    python aim_scan.py peek <addr> [count]            print floats at an address

Pitch is the engine's own, radians, positive up, as `asin(in_fwd.z)` from `ev=vr.probe pose`.
Only reads (ReadProcessMemory). Samples live in %TEMP%\\claude, never in the repo.
"""
import ctypes
import math
import os
import pickle
import struct
import subprocess
import sys
from ctypes import wintypes

import numpy as np

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.OpenProcess.restype = wintypes.HANDLE
k32.ReadProcessMemory.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                  ctypes.POINTER(ctypes.c_size_t)]
k32.VirtualQueryEx.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t]
k32.VirtualQueryEx.restype = ctypes.c_size_t

TMP = os.path.join(os.environ.get("TEMP", "."), "claude")


class MBI(ctypes.Structure):
    _fields_ = [("BaseAddress", ctypes.c_void_p), ("AllocationBase", ctypes.c_void_p),
                ("AllocationProtect", wintypes.DWORD), ("PartitionId", wintypes.WORD),
                ("RegionSize", ctypes.c_size_t), ("State", wintypes.DWORD), ("Protect", wintypes.DWORD),
                ("Type", wintypes.DWORD)]


def pid_of(name="destiny2.exe"):
    out = subprocess.check_output(["tasklist", "/FI", f"IMAGENAME eq {name}", "/FO", "CSV"]).decode(errors="ignore")
    for line in out.splitlines()[1:]:
        parts = line.strip('"').split('","')
        if len(parts) > 1:
            return int(parts[1])
    sys.exit("destiny2.exe not running")


def handle():
    return k32.OpenProcess(0x0410, False, pid_of())


def read(h, addr, size):
    buf = ctypes.create_string_buffer(size)
    got = ctypes.c_size_t(0)
    if not k32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, size, ctypes.byref(got)):
        return None
    return buf.raw[: got.value]


def regions(h, max_region=256 << 20):
    addr = 0
    mbi = MBI()
    while k32.VirtualQueryEx(h, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)):
        base = mbi.BaseAddress or 0
        size = mbi.RegionSize
        # MEM_COMMIT, MEM_PRIVATE, PAGE_READWRITE only: heap and stacks, where a character lives.
        if mbi.State == 0x1000 and mbi.Type == 0x20000 and mbi.Protect == 0x04 and size <= max_region:
            yield base, size
        addr = base + size
        if addr >= 0x7FFFFFFFFFFF:
            break


def encodings(p0, p1):
    return {"rad": p1 - p0, "deg": math.degrees(p1 - p0), "sin": math.sin(p1) - math.sin(p0),
            "cos": math.cos(p1) - math.cos(p0), "-rad": p0 - p1, "-sin": math.sin(p0) - math.sin(p1)}


def match(a, b, p0, p1, tol=0.06):
    """Indices where b - a matches one encoding of the aim change, and the absolute value is plausible."""
    d = b - a
    hits = {}
    for name, want in encodings(p0, p1).items():
        if abs(want) < 1e-3:
            continue
        ok = np.isfinite(a) & np.isfinite(b) & (np.abs(d - want) < abs(want) * tol)
        if name in ("rad", "-rad"):
            ok &= np.abs(b) < 7.0
        if name == "deg":
            ok &= np.abs(b) < 400.0
        if name in ("sin", "cos", "-sin"):
            ok &= np.abs(b) <= 1.0001
        for i in np.nonzero(ok)[0]:
            hits.setdefault(int(i), []).append(name)
    return hits


def cmd_block(addr, op, pitch):
    h = handle()
    path = os.path.join(TMP, "aim_block.pkl")
    data = np.frombuffer(read(h, addr, 0x1000), dtype="<f4").astype(np.float64)
    if op == "save":
        pickle.dump({"pitch": pitch, "f": data}, open(path, "wb"))
        print(f"saved block at pitch {pitch:.4f}")
        return
    old = pickle.load(open(path, "rb"))
    hits = match(old["f"], data, old["pitch"], pitch)
    print(f"pitch {old['pitch']:.4f} -> {pitch:.4f}")
    for i, names in sorted(hits.items()):
        print(f"  +0x{i * 4:03X}  {old['f'][i]:+.5f} -> {data[i]:+.5f}  {','.join(names)}")
    print(f"{len(hits)} candidate(s)")


def cmd_heap(op, pitch):
    h = handle()
    path = os.path.join(TMP, "aim_heap.pkl")
    if op == "save":
        chunks = []
        total = 0
        for base, size in regions(h):
            raw = read(h, base, size)
            if not raw:
                continue
            chunks.append((base, np.frombuffer(raw, dtype="<f4").copy()))
            total += len(raw)
        pickle.dump({"pitch": pitch, "chunks": chunks}, open(path, "wb"), protocol=4)
        print(f"saved {len(chunks)} regions, {total / 1e6:.0f} MB, at pitch {pitch:.4f}")
        return
    old = pickle.load(open(path, "rb"))
    cand_path = os.path.join(TMP, "aim_cand.pkl")
    if op == "cmp":
        cands = []
        for base, arr in old["chunks"]:
            raw = read(h, base, arr.size * 4)
            if not raw or len(raw) != arr.size * 4:
                continue
            new = np.frombuffer(raw, dtype="<f4")
            a = arr.astype(np.float64)
            b = new.astype(np.float64)
            for i, names in match(a, b, old["pitch"], pitch).items():
                cands.append((base + 4 * i, names, float(a[i]), float(b[i])))
        pickle.dump({"pitch": pitch, "cands": cands, "p0": old["pitch"]}, open(cand_path, "wb"))
        print(f"{len(cands)} candidate(s)")
        for c in cands[:60]:
            print(f"  {c[0]:#x} {c[2]:+.5f} -> {c[3]:+.5f} {','.join(c[1])}")
        return
    if op == "again":
        prev = pickle.load(open(cand_path, "rb"))
        keep = []
        p0 = prev["p0"]
        for addr, names, a, b in prev["cands"]:
            raw = read(h, addr, 4)
            if not raw:
                continue
            v = struct.unpack("<f", raw)[0]
            enc = encodings(p0, pitch)
            ok = [n for n in names if abs((v - a) - enc[n]) < max(abs(enc[n]) * 0.06, 1e-3)]
            if ok:
                keep.append((addr, ok, a, v))
        pickle.dump({"pitch": pitch, "cands": keep, "p0": p0}, open(cand_path, "wb"))
        print(f"{len(keep)} candidate(s) survive")
        for c in keep[:80]:
            print(f"  {c[0]:#x} {c[2]:+.5f} -> {c[3]:+.5f} {','.join(c[1])}")


def values_for(p):
    return {"rad": p, "-rad": -p, "deg": math.degrees(p), "sin": math.sin(p), "-sin": -math.sin(p),
            "cos": math.cos(p)}


def cmd_find(op, pitch, tol=2e-3):
    """Classic scanner: `first` keeps every float equal to some encoding of the aim, `next` keeps the
    survivors whose value moved to the same encoding of the new aim. Cheap in memory: only matches
    are kept, never the heap."""
    h = handle()
    path = os.path.join(TMP, "aim_find.pkl")
    want = values_for(pitch)
    if op == "first":
        keep = []
        total = 0
        for base, size in regions(h):
            raw = read(h, base, size)
            if not raw:
                continue
            total += len(raw)
            arr = np.frombuffer(raw[: len(raw) // 4 * 4], dtype="<f4")
            for name, v in want.items():
                if name in ("cos",) and abs(v) > 0.999:
                    continue  # cos near 1 matches every unit value; useless at small pitch
                t = tol * (57.3 if name == "deg" else 1.0)
                idx = np.nonzero(np.abs(arr - v) < t)[0]
                keep.extend((base + 4 * int(i), name) for i in idx)
        pickle.dump(keep, open(path, "wb"))
        print(f"scanned {total / 1e6:.0f} MB, {len(keep)} candidate(s)")
        return
    keep = pickle.load(open(path, "rb"))
    out = []
    for addr, name in keep:
        raw = read(h, addr, 4)
        if not raw:
            continue
        v = struct.unpack("<f", raw)[0]
        t = tol * (57.3 if name == "deg" else 1.0)
        if abs(v - want[name]) < t:
            out.append((addr, name))
    pickle.dump(out, open(path, "wb"))
    print(f"{len(out)} candidate(s) survive")
    for addr, name in out[:80]:
        print(f"  {addr:#x} {name}")


def cmd_poke(addr, value, repeat_ms=0):
    """Writes one float (WriteProcessMemory: no exception can reach the game). With repeat_ms, keeps
    rewriting it for that long, to tell a value the engine reads from one it merely recomputes."""
    import time
    k32.WriteProcessMemory.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                       ctypes.POINTER(ctypes.c_size_t)]
    h = k32.OpenProcess(0x0438, False, pid_of())
    data = struct.pack("<f", value)
    end = time.time() + repeat_ms / 1000.0
    n = 0
    while True:
        wrote = ctypes.c_size_t(0)
        k32.WriteProcessMemory(h, ctypes.c_void_p(addr), data, 4, ctypes.byref(wrote))
        n += 1
        if time.time() >= end:
            break
        time.sleep(0.002)
    print(f"wrote {value} to {addr:#x} x{n}")


def cmd_probe(block, target):
    """For every surviving candidate: hold it at `target` (in its own encoding) for a moment while
    watching the engine's inner camera forward.z (block+0x9A0, which follows the character's pitch).
    A candidate the engine reads moves the camera; a derived copy does not."""
    import time
    k32.WriteProcessMemory.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                       ctypes.POINTER(ctypes.c_size_t)]
    h = k32.OpenProcess(0x0438, False, pid_of())
    cands = pickle.load(open(os.path.join(TMP, "aim_find.pkl"), "rb"))
    want = values_for(target)

    def fz():
        return struct.unpack("<f", read(h, block + 0x9A0, 4))[0]

    for addr, name in cands:
        # Thread stacks live far below the heap here; a float poked into a live stack frame is a
        # crash waiting to happen, and an aim angle is not kept on a stack between frames anyway.
        if addr < 0x10000000000:
            continue
        orig = struct.unpack("<f", read(h, addr, 4))[0]
        z0 = fz()
        data = struct.pack("<f", want[name])
        end = time.time() + 0.6
        wrote = ctypes.c_size_t(0)
        while time.time() < end:
            k32.WriteProcessMemory(h, ctypes.c_void_p(addr), data, 4, ctypes.byref(wrote))
            time.sleep(0.002)
        z1 = fz()
        after = struct.unpack("<f", read(h, addr, 4))[0]
        k32.WriteProcessMemory(h, ctypes.c_void_p(addr), struct.pack("<f", orig), 4, ctypes.byref(wrote))
        moved = abs(z1 - z0) > 0.02
        print(f"  {addr:#x} {name:5} orig={orig:+.4f} held={want[name]:+.4f} now={after:+.4f} "
              f"fwd.z {z0:+.4f}->{z1:+.4f} {'<-- MOVES THE CAMERA' if moved else ''}", flush=True)
        if moved:
            time.sleep(1.0)


def cmd_peek(addr, count):
    h = handle()
    raw = read(h, addr, count * 4)
    vals = struct.unpack(f"<{len(raw) // 4}f", raw)
    for i in range(0, len(vals), 4):
        print(f"  {addr + 4 * i:#x}: " + " ".join(f"{v:+12.5f}" for v in vals[i:i + 4]))


def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__)
    elif a[0] == "block":
        cmd_block(int(a[1], 16), a[2], float(a[3]))
    elif a[0] == "heap":
        cmd_heap(a[1], float(a[2]))
    elif a[0] == "poke":
        cmd_poke(int(a[1], 16), float(a[2]), int(a[3]) if len(a) > 3 else 0)
    elif a[0] == "probe":
        cmd_probe(int(a[1], 16), float(a[2]))
    elif a[0] == "find":
        cmd_find(a[1], float(a[2]))
    elif a[0] == "peek":
        cmd_peek(int(a[1], 16), int(a[2]) if len(a) > 2 else 16)


if __name__ == "__main__":
    main()
