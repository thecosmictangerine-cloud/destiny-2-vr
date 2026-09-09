"""Tiles the weapon's corner of several captures onto one labelled sheet.

Template matching stops working once the weapon's orientation is being driven too, because the gun
no longer looks like its own template. When that happens the honest instrument is a picture, and a
contact sheet makes a whole test matrix readable in one look instead of twenty.

Usage: python contact_sheet.py <shots dir> <tag> <out.png> [--columns N] [--full]
"""

import re
import sys
from pathlib import Path

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
ZOOM_FRACTION = (0.34, 0.28, 0.97, 1.00)


def main():
    directory = Path(sys.argv[1])
    tag = sys.argv[2]
    out = sys.argv[3]
    columns = int(sys.argv[sys.argv.index("--columns") + 1]) if "--columns" in sys.argv else 4
    full = "--full" in sys.argv

    # The tag is a prefix, not a whole field, so a run can be sliced (w1_0 for the first nine steps).
    shots = sorted(directory.glob(f"{tag}*.png"))
    shots = [s for s in shots if not s.name.startswith("panel")]
    if not shots:
        raise SystemExit(f"no shots matching {tag}_*.png")

    cell_w = 1520 // columns
    tiles = []
    for shot in shots:
        image = Image.open(shot).convert("RGB").crop(CLIENT)
        if not full:
            image = image.crop(_zoom(CLIENT))
        scale = cell_w / float(image.width)
        tiles.append((shot.name, image.resize((cell_w, int(image.height * scale)))))

    cell_h = tiles[0][1].height
    label_h = 18
    rows = (len(tiles) + columns - 1) // columns
    sheet = Image.new("RGB", (cell_w * columns, (cell_h + label_h) * rows), (250, 250, 250))
    draw = ImageDraw.Draw(sheet)
    for index, (name, tile) in enumerate(tiles):
        column, row = index % columns, index // columns
        x, y = column * cell_w, row * (cell_h + label_h)
        # Drop the tag and the extension: the step name is the only part worth reading.
        label = re.sub(rf"^{re.escape(tag)}_", "", name).replace(".png", "")
        draw.text((x + 4, y + 3), label, fill=(0, 0, 0))
        sheet.paste(tile, (x, y + label_h))
        draw.rectangle([x, y + label_h, x + cell_w - 1, y + label_h + cell_h - 1], outline=(180, 180, 180))
    sheet.save(directory / out)
    print(f"wrote {directory / out} ({sheet.width}x{sheet.height}) from {len(tiles)} shots")


if __name__ == "__main__":
    main()
