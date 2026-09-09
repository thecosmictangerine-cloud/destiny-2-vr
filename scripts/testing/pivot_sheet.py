"""Blends each level/pitched pair into one panel so the weapon's centre of rotation is visible.

Two frames of the same scene differing only in the hand's pitch are combined into a single image:
the level weapon in one colour channel, the pitched weapon in another. Where the two agree the pixel
stays grey; where only one has the weapon it takes on a colour. So a weapon that rotates about a
FIXED POINT shows two coloured blades meeting at a grey hinge, and a weapon that swings shows two
blades with no common point at all.

This is deliberately a picture rather than a number. Every numeric route to the same quantity was
tried first and each was defeated by the same thing: the gun's silhouette changes when it rotates,
and cross-correlation answers with the offset that best fits a changed shape rather than with the
translation. That bias reached about 100 px, which is the size of the effect being measured. A hinge
is not fooled by foreshortening.

Usage: python pivot_sheet.py <shots dir> <pairs.csv>
"""

import csv
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

# The weapon's corner of the frame, as fractions of the client area, generous enough to hold the
# barrel when it swings up to near vertical.
CROP = (0.52, 0.34, 0.98, 1.00)


def _client_rect():
    import os
    import re
    match = re.match(r"^\s*(-?\d+),(-?\d+),(\d+),(\d+)\s*$", os.environ.get("SVR_CLIENT_RECT", ""))
    if match:
        x, y, w, h = (int(g) for g in match.groups())
        return (max(0, x), max(0, y), x + w, y + h)
    return (0, 0, 1920, 1080)


def panel(directory, level_name, pitched_name, label):
    client = _client_rect()
    level = Image.open(directory / level_name).convert("L").crop(client)
    pitched = Image.open(directory / pitched_name).convert("L").crop(client)
    width, height = level.size
    box = (int(CROP[0] * width), int(CROP[1] * height), int(CROP[2] * width), int(CROP[3] * height))
    a = np.asarray(level.crop(box), dtype=np.float32)
    b = np.asarray(pitched.crop(box), dtype=np.float32)

    # Grey where the frames agree, coloured where only one frame has the weapon. The weapon is DARK
    # against Io's pale ground, so the frame holding it is the DARKER one -- which is why the sign
    # below looks inverted and why the legend was wrong the first time: RED marks the pitched
    # weapon, BLUE the level one.
    difference = a - b
    base = (a + b) * 0.5
    red = np.clip(base + np.clip(difference, 0, None) * 1.4, 0, 255)
    green = np.clip(base - np.abs(difference) * 0.25, 0, 255)
    blue = np.clip(base + np.clip(-difference, 0, None) * 1.4, 0, 255)
    rgb = np.stack([red, green, blue], axis=2).astype(np.uint8)
    image = Image.fromarray(rgb)

    scale = 560.0 / image.width
    image = image.resize((560, int(image.height * scale)))
    draw = ImageDraw.Draw(image)
    draw.rectangle((0, 0, image.width - 1, 22), fill=(0, 0, 0))
    draw.text((6, 6), label + "   (blue = level, red = pitched)", fill=(255, 255, 255))
    return image


def main():
    directory = Path(sys.argv[1])
    rows = []
    with open(sys.argv[2], newline="", encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            rows.append(row)
    panels = [panel(directory, r["Level"], r["Pitched"], f"pivot fwd {r['Pivot']} m") for r in rows]
    if not panels:
        print("pivot_sheet: nothing to draw")
        return 1
    columns = 2
    rows_count = (len(panels) + columns - 1) // columns
    cell_w, cell_h = panels[0].width, panels[0].height
    sheet = Image.new("RGB", (columns * cell_w, rows_count * cell_h), (16, 16, 16))
    for index, image in enumerate(panels):
        sheet.paste(image, ((index % columns) * cell_w, (index // columns) * cell_h))
    out = directory / (rows[0]["Level"].split("_")[0] + "_sheet.png")
    sheet.save(out)
    print(f"  wrote {out.name}  ({sheet.width}x{sheet.height}, {len(panels)} panels)")
    print("  RED = weapon pitched, BLUE = weapon level (the weapon is dark, so it darkens its own frame).")
    print("  The right pivot is the panel where the two meet at a common point near the GRIP.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
