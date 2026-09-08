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

CLIENT = (9, 38, 9 + 1280, 38 + 720)
# The weapon's corner of the client area: enough room for the gun to travel a long way.
ZOOM = (440, 200, 1240, 720)


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
            image = image.crop(ZOOM)
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
