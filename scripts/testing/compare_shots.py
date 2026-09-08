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

CLIENT = (9, 38, 9 + 1280, 38 + 720)
# The weapon's corner of the client area, for --zoom.
ZOOM = (560, 300, 1180, 720)


def load(path, zoom):
    image = Image.open(path).convert("RGB").crop(CLIENT)
    return image.crop(ZOOM) if zoom else image


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
