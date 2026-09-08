"""Looks for a writable body-yaw value in the engine's inner camera state.

Why this matters: the camera/body work needs the character's facing to follow the head. The only
lever known to work is injecting mouse motion, which needs a servo because mouse counts per degree
are not repeatable between sessions. If instead the engine keeps the body's yaw as a plain angle
somewhere in the camera block, that angle can be written directly -- exact, deterministic, and with
no injected input at all. One access watch already established that the engine recomputes the
block's pose from an inner state around `block+0x96C..+0x9A4`, so that is where to look.

Method: sample a window of the block at two known body yaws and report every float whose change
matches the yaw change. Radians, degrees and the sine and cosine of the angle are all checked,
because any of the four is a plausible storage form.

    python find_body_yaw.py <block address> <yaw before> <yaw after>

NOT YET RUN. The servo in `vr_camera.cpp` reached the same goal first -- it learns its own
mouse-counts-per-radian and settles to about half a degree -- so this was never needed. It is kept
because a direct write would still be strictly better: exact, deterministic, and with no injected
input at all, which would also give the physical mouse back. Treat it as a sketch, not as a tool
known to work.

The block address comes from `ev=vr.probe block addr=` in the log; the yaws from
`ev=vr.probe pose in_fwd=` as atan2(fwd.y, fwd.x). Run it twice around a deliberate turn:
`--save` writes the first sample, then `--compare` reads it back and does the matching.
"""

import ctypes
import json
import math
import struct
import subprocess
import sys
from ctypes import wintypes
from pathlib import Path

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.OpenProcess.restype = wintypes.HANDLE
k32.ReadProcessMemory.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                                  ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]

# Window of the block to sample, as offsets. Wide enough to cover the inner state the watch found
# plus its neighbourhood, since a yaw accumulator need not sit inside the pose itself.
WINDOW = (0x500, 0xB00)
STATE = Path(__file__).with_name("body_yaw_sample.json")


def pid_of(name="destiny2.exe"):
    out = subprocess.check_output(["tasklist", "/FI", f"IMAGENAME eq {name}", "/FO", "CSV"])
    for line in out.decode(errors="ignore").splitlines()[1:]:
        parts = [p.strip('"') for p in line.split('","')]
        if len(parts) > 1:
            return int(parts[1])
    raise SystemExit("destiny2.exe is not running")


def sample(block):
    handle = k32.OpenProcess(0x0410, False, pid_of())
    if not handle:
        raise SystemExit("OpenProcess failed")
    size = WINDOW[1] - WINDOW[0]
    buffer = ctypes.create_string_buffer(size)
    got = ctypes.c_size_t(0)
    ok = k32.ReadProcessMemory(handle, ctypes.c_void_p(block + WINDOW[0]), buffer, size,
                               ctypes.byref(got))
    if not ok or got.value != size:
        raise SystemExit(f"read failed at 0x{block + WINDOW[0]:X}")
    return list(struct.unpack("<%df" % (size // 4), buffer.raw))


def main():
    block = int(sys.argv[1], 16)
    if "--save" in sys.argv:
        yaw = float(sys.argv[2])
        STATE.write_text(json.dumps({"block": block, "yaw": yaw, "floats": sample(block)}))
        print(f"saved {len(sample(block))} floats at yaw {yaw:.4f} -> {STATE.name}")
        return

    before = json.loads(STATE.read_text())
    yaw_before = before["yaw"]
    yaw_after = float(sys.argv[2])
    after = sample(block)
    old = before["floats"]

    delta_yaw = math.atan2(math.sin(yaw_after - yaw_before), math.cos(yaw_after - yaw_before))
    print(f"yaw {yaw_before:.4f} -> {yaw_after:.4f}  (delta {delta_yaw:+.4f} rad, "
          f"{math.degrees(delta_yaw):+.2f} deg)")
    candidates = [
        ("radians", delta_yaw),
        ("degrees", math.degrees(delta_yaw)),
        ("sin", math.sin(yaw_after) - math.sin(yaw_before)),
        ("cos", math.cos(yaw_after) - math.cos(yaw_before)),
    ]
    print(f"{'offset':>8} {'before':>12} {'after':>12} {'change':>12}  matches")
    hits = 0
    for index, (a, b) in enumerate(zip(old, after)):
        if not (math.isfinite(a) and math.isfinite(b)):
            continue
        change = b - a
        if abs(change) < 1.0e-4:
            continue
        names = [name for name, expected in candidates
                 if abs(expected) > 1.0e-4 and abs(change - expected) < abs(expected) * 0.08]
        if names:
            hits += 1
            offset = WINDOW[0] + index * 4
            print(f"  +0x{offset:03X} {a:>12.5f} {b:>12.5f} {change:>+12.5f}  {','.join(names)}")
    print(f"{hits} candidate(s)")


if __name__ == "__main__":
    main()
