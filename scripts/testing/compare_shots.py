"""Builds one image that answers one question, so a comparison costs one look instead of several.

Two captures are cropped to the game's client area, scaled down, and stacked with a difference
panel underneath. The difference is amplified and inverted so that anything which moved shows as
dark ink on white: a gun that stayed put leaves its outline blank, a gun that moved leaves a
doubled ghost.

Usage: python compare_shots.py <shots dir> <a.png> <b.png> <out.png> [--zoom]
       --zoom crops to the weapon's corner of the frame instead of the whole client area.
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
# The weapon's corner of the client area for --zoom, as fractions so it follows the window size.
ZOOM_FRACTION = (0.44, 0.42, 0.92, 1.00)


def load(path, zoom):
    image = Image.open(path).convert("RGB").crop(CLIENT)
    if not zoom:
        return image
    left, top, right, bottom = ZOOM_FRACTION
    return image.crop((int(left * image.width), int(top * image.height),
                       int(right * image.width), int(bottom * image.height)))


def main():
    directory = Path(sys.argv[1])
    a_path, b_path, out = sys.argv[2], sys.argv[3], sys.argv[4]
    zoom = "--zoom" in sys.argv

    a = load(directory / a_path, zoom)
    b = load(directory / b_path, zoom)
    scale = 760.0 / a.width
    size = (int(a.width * scale), int(a.height * scale))
    a_small, b_small = a.resize(size), b.resize(size)

    # Amplified, inverted difference: white where nothing changed, dark where something moved.
    delta = np.abs(np.asarray(a_small, dtype=np.int16) - np.asarray(b_small, dtype=np.int16))
    delta = 255 - np.clip(delta.mean(axis=2) * 4.0, 0, 255)
    diff = Image.fromarray(delta.astype(np.uint8)).convert("RGB")

    label = 22
    sheet = Image.new("RGB", (size[0] * 2, size[1] * 2 + label * 3), (255, 255, 255))
    draw = ImageDraw.Draw(sheet)
    sheet.paste(a_small, (0, label))
    sheet.paste(b_small, (size[0], label))
    sheet.paste(diff, (0, size[1] + label * 2))
    draw.text((4, 5), "A: " + a_path, fill=(0, 0, 0))
    draw.text((size[0] + 4, 5), "B: " + b_path, fill=(0, 0, 0))
    draw.text((4, size[1] + label + 5), "difference x4, inverted: dark = moved", fill=(0, 0, 0))
    sheet.save(directory / out)
    print(f"wrote {directory / out}  ({sheet.width}x{sheet.height})")


if __name__ == "__main__":
    main()
