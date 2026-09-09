"""Puts several A/B difference panels on one sheet, so several questions cost one look.

Whole-frame metrics turned out to be unusable across a long session: Io's lighting drifts, and the
weapon's own idle animation moves it a few pixels, so a "rest" measurement taken minutes apart can
differ by tens of pixels. Differencing two captures taken seconds apart does not have that problem,
and the panel makes the answer visual instead of statistical.

Each panel amplifies |A - B| and inverts it, so:
  blank white where the pixels are identical -- that part of the frame did not move
  dark ink where they differ -- that part moved

A pair with nothing changed between the captures is the control: whatever ink it shows is the
scene's own animation, and any real effect has to be clearly stronger than that.

Usage: python diff_panel.py <shots dir> <out.png> <label>=<a.png>,<b.png> [more...]
"""

import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

def _client_rect():
    """@return The game's client area inside a full-desktop capture, as (left, top, right, bottom).

    Read from `SVR_CLIENT_RECT` (`x,y,w,h`), which the PowerShell tests export after asking Windows
    for the real rectangle. A hardcoded constant was wrong the moment the window resolution changed
    and silently so -- the crops still produced pictures, just of the wrong part of the frame -- so
    the assumption lives in one place now, and the fallback says what it assumes.
    """
    import os
    import re
    match = re.match(r"^\s*(-?\d+),(-?\d+),(\d+),(\d+)\s*$", os.environ.get("SVR_CLIENT_RECT", ""))
    if match:
        x, y, w, h = (int(g) for g in match.groups())
        return (max(0, x), max(0, y), x + w, y + h)
    # A 1920x1080 window at 0,0 on a 1920x1080 desktop, which is the project default; the client
    # starts below the title bar and the bottom is clipped by the screen.
    return (0, 31, 1920, 1080)


CLIENT = _client_rect()


def _zoom(client):
    """@return ZOOM_FRACTION resolved to pixels inside a client area of this size."""
    width, height = client[2] - client[0], client[3] - client[1]
    left, top, right, bottom = ZOOM_FRACTION
    return (int(left * width), int(top * height), int(right * width), int(bottom * height))
# The weapon's corner of the client area, as fractions, so it follows the window size.
ZOOM_FRACTION = (0.41, 0.36, 0.94, 1.00)
COLUMNS = 3
AMPLIFY = 4.0


def panel(directory, a_name, b_name, width, full=False):
    a = Image.open(directory / a_name).convert("RGB").crop(CLIENT)
    b = Image.open(directory / b_name).convert("RGB").crop(CLIENT)
    if not full:
        a, b = a.crop(_zoom(CLIENT)), b.crop(_zoom(CLIENT))
    scale = width / float(a.width)
    size = (int(a.width * scale), int(a.height * scale))
    delta = np.abs(np.asarray(a.resize(size), dtype=np.int16)
                   - np.asarray(b.resize(size), dtype=np.int16))
    ink = 255 - np.clip(delta.mean(axis=2) * AMPLIFY, 0, 255)
    return Image.fromarray(ink.astype(np.uint8)).convert("RGB"), float(delta.mean())


def main():
    directory = Path(sys.argv[1])
    out = sys.argv[2]
    # --full keeps the whole client area, for when the effect under test might carry the weapon
    # outside the zoom window and "moved a long way" has to be told from "vanished".
    full = "--full" in sys.argv
    pairs = []
    for argument in sys.argv[3:]:
        if argument.startswith("--"):
            continue
        # rsplit, not split: labels themselves may contain "=".
        label, names = argument.rsplit("=", 1)
        a_name, b_name = names.split(",", 1)
        pairs.append((label, a_name, b_name))

    width = 470
    panels = [(label,) + panel(directory, a, b, width, full) for label, a, b in pairs]
    rows = (len(panels) + COLUMNS - 1) // COLUMNS
    cell_h = panels[0][1].height
    label_h = 20
    sheet = Image.new("RGB", (width * COLUMNS, (cell_h + label_h) * rows), (255, 255, 255))
    draw = ImageDraw.Draw(sheet)
    for index, (label, image, mad) in enumerate(panels):
        column, row = index % COLUMNS, index // COLUMNS
        x, y = column * width, row * (cell_h + label_h)
        draw.text((x + 4, y + 4), f"{label}   (mad {mad:.2f})", fill=(0, 0, 0))
        sheet.paste(image, (x, y + label_h))
    sheet.save(directory / out)
    print(f"wrote {directory / out} ({sheet.width}x{sheet.height})")
    for label, _, mad in panels:
        print(f"  {label:<28} mean abs diff {mad:.2f}")


if __name__ == "__main__":
    main()
