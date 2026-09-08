"""Measures where the weapon is drawn and how big it is, per screenshot.

Whole-frame differencing is useless here: Io's grass, blinking lights and the HUD move on their own
and put the noise floor around five grey levels, which is the same size as the effects being
looked for. So instead the weapon is segmented and reduced to three numbers that the scene's
animation cannot fake.

The segmentation is a colour test, not a brightness one. The ground and grass are cyan-green, so
`red - (green+blue)/2` is strongly negative for them, while the gun (dark grey with red panels)
and the forearm (near-neutral grey) sit near or above zero. Restricting that test to the window
the viewmodel is drawn in keeps the distant structures and the HUD out of it.

What the numbers mean:
  cx, cy   centroid of the weapon mask, in client pixels: where the gun is on screen
  area     mask pixel count: how big it is, so approach and recession are visible
  fill     area as a share of the window, for sanity

Usage: python weapon_metric.py <shots dir> <tag> [more tags...]
"""

import sys
from pathlib import Path

import numpy as np
from PIL import Image

# Client area of the game window inside the full-desktop capture, physical pixels.
CLIENT = (9, 38, 9 + 1280, 38 + 720)
# The window the first-person weapon is drawn in, as fractions of the client area. Chosen from a
# coarse map of the mask: the gun and forearm occupy roughly x 0.60-0.80, y 0.55-1.00, and this
# window is wide enough to let the gun travel a long way before leaving it.
WINDOW = (0.50, 0.52, 0.92, 1.00)
# Above this, `red - (green+blue)/2` means neutral or red, i.e. gun or arm rather than ground.
REDNESS = -15.0


def measure(path):
    """@return Centroid, area and fill of the weapon mask in one capture."""
    image = Image.open(path).convert("RGB").crop(CLIENT)
    array = np.asarray(image).astype(np.int16)
    height, width, _ = array.shape
    x0, y0, x1, y1 = (int(WINDOW[0] * width), int(WINDOW[1] * height),
                      int(WINDOW[2] * width), int(WINDOW[3] * height))
    window = array[y0:y1, x0:x1]
    red, green, blue = window[:, :, 0], window[:, :, 1], window[:, :, 2]
    mask = (red - (green + blue) / 2.0) > REDNESS
    area = int(mask.sum())
    if area == 0:
        return None
    ys, xs = np.nonzero(mask)
    return {
        "cx": float(xs.mean()) + x0,
        "cy": float(ys.mean()) + y0,
        "area": area,
        "fill": area / float(mask.size) * 100.0,
    }


def main():
    directory = Path(sys.argv[1])
    for tag in sys.argv[2:]:
        shots = sorted(directory.glob(f"{tag}_*.png"))
        if not shots:
            print(f"{tag}: no shots")
            continue
        print(f"\n== {tag} ==")
        base = measure(shots[0])
        print(f"{'shot':<34} {'cx':>7} {'cy':>7} {'area':>8} {'dcx':>7} {'dcy':>7} {'darea%':>8}")
        for shot in shots:
            m = measure(shot)
            if m is None:
                print(f"{shot.name:<34} (empty mask)")
                continue
            print(f"{shot.name:<34} {m['cx']:>7.1f} {m['cy']:>7.1f} {m['area']:>8d} "
                  f"{m['cx'] - base['cx']:>+7.1f} {m['cy'] - base['cy']:>+7.1f} "
                  f"{(m['area'] / base['area'] - 1) * 100:>+7.1f}%")


if __name__ == "__main__":
    main()
