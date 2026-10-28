"""Locates the game's own pointer in a set of desktop screenshots taken with the system pointer at
known places (Probe-Cursor.ps1): each shot is differenced against the median of all of them, and the
arrow is the bright blob left over (its tip is the top-left of the blob).

    python cursor_find.py <shots dir> <tag> <n>
"""
import sys
import numpy as np
from PIL import Image

def main():
    folder, tag, n = sys.argv[1], sys.argv[2], int(sys.argv[3])
    shots = [np.asarray(Image.open(f"{folder}/{tag}_{i}.png").convert("RGB"), dtype=np.int16) for i in range(n)]
    median = np.median(np.stack(shots), axis=0)
    for i, shot in enumerate(shots):
        diff = np.abs(shot - median).sum(axis=2)
        mask = diff > 120
        ys, xs = np.nonzero(mask)
        if len(xs) == 0:
            print(f"shot {i}: nothing")
            continue
        # The largest compact cluster: take points within 40 px of the densest point.
        cx, cy = np.median(xs), np.median(ys)
        near = (np.abs(xs - cx) < 40) & (np.abs(ys - cy) < 40)
        xs, ys = xs[near], ys[near]
        print(f"shot {i}: tip {xs.min()},{ys.min()} centre {xs.mean():.0f},{ys.mean():.0f} pixels {len(xs)}")

main()
