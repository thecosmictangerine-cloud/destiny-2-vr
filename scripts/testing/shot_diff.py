"""Finds what a shot left in the frame: the difference between a capture before it and one after.

    python shot_diff.py <before.png> <after.png> <out.png> [--mask x0,y0,x1,y1 ...] [--json]

Writes a sheet with the after frame and the amplified difference, and prints the blobs of change
that survive masking: the weapon (it recoils), the HUD's ammo counter and anything passed with
--mask are excluded, so what remains is the world -- an impact decal, its smoke, its sparks.

The difference is taken per pixel on the max channel, blurred a little so a decal reads as one blob
rather than a speckle, thresholded against the frame's own noise (median + k * MAD), and the
connected regions are reported largest first with their centroid and area. Captures must be
seconds apart: Io's lighting drifts over minutes.
"""
import json
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter


def load(path):
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.int16)


def blobs(mask, min_area=12):
    """Connected components (4-neighbour) by flood fill on a downsampled grid; small and dependency-free."""
    h, w = mask.shape
    seen = np.zeros_like(mask, dtype=bool)
    out = []
    ys, xs = np.nonzero(mask)
    for y0, x0 in zip(ys, xs):
        if seen[y0, x0]:
            continue
        stack = [(y0, x0)]
        seen[y0, x0] = True
        pts = []
        while stack:
            y, x = stack.pop()
            pts.append((y, x))
            for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                ny, nx = y + dy, x + dx
                if 0 <= ny < h and 0 <= nx < w and mask[ny, nx] and not seen[ny, nx]:
                    seen[ny, nx] = True
                    stack.append((ny, nx))
        if len(pts) >= min_area:
            arr = np.array(pts)
            out.append({"area": len(pts), "cy": float(arr[:, 0].mean()), "cx": float(arr[:, 1].mean()),
                        "y0": int(arr[:, 0].min()), "y1": int(arr[:, 0].max()),
                        "x0": int(arr[:, 1].min()), "x1": int(arr[:, 1].max())})
    out.sort(key=lambda b: -b["area"])
    return out


def main():
    args = sys.argv[1:]
    before, after, out = args[0], args[1], args[2]
    masks = []
    as_json = "--json" in args
    k = 6.0
    for i, a in enumerate(args):
        if a == "--mask":
            masks.append(tuple(int(v) for v in args[i + 1].split(",")))
        if a == "--k":
            k = float(args[i + 1])
    A = load(before)
    B = load(after)
    d = np.abs(A - B).max(axis=2).astype(np.float32)
    for x0, y0, x1, y1 in masks:
        d[y0:y1, x0:x1] = 0
    img = Image.fromarray(np.clip(d, 0, 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(2))
    s = np.asarray(img, dtype=np.float32)
    med = float(np.median(s))
    mad = float(np.median(np.abs(s - med))) + 1e-3
    thr = max(med + k * 1.4826 * mad, 12.0)
    # Downsample by 4 for the component search: decals are tens of pixels, so nothing is lost.
    small = s[::4, ::4] > thr
    found = blobs(small, min_area=3)
    for b in found:
        for key in ("cy", "cx", "y0", "y1", "x0", "x1"):
            b[key] = b[key] * 4
        b["area"] *= 16
    found = found[:8]
    sheet = Image.new("RGB", (B.shape[1], B.shape[0] * 2 // 2 + B.shape[0] // 2), "white")
    after_img = Image.open(after).convert("RGB")
    amp = Image.fromarray(255 - np.clip(s * 4, 0, 255).astype(np.uint8)).convert("RGB")
    half = (B.shape[1] // 2, B.shape[0] // 2)
    sheet = Image.new("RGB", (half[0] * 2, half[1]), "white")
    sheet.paste(after_img.resize(half), (0, 0))
    sheet.paste(amp.resize(half), (half[0], 0))
    draw = ImageDraw.Draw(sheet)
    for n, b in enumerate(found):
        for ox in (0, half[0]):
            draw.rectangle([ox + b["x0"] / 2 - 4, b["y0"] / 2 - 4, ox + b["x1"] / 2 + 4, b["y1"] / 2 + 4],
                           outline=(255, 0, 0) if n == 0 else (255, 160, 0), width=2)
    sheet.save(out)
    if as_json:
        print(json.dumps({"threshold": thr, "blobs": found}))
    else:
        print(f"threshold {thr:.1f}")
        for b in found:
            print(f"  blob area={b['area']:6d} centre=({b['cx']:.0f},{b['cy']:.0f}) box=({b['x0']},{b['y0']})-({b['x1']},{b['y1']})")


if __name__ == "__main__":
    main()
