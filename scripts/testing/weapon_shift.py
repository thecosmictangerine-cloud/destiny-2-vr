"""Measures how far the weapon moved between two captures, in pixels, by template matching.

Colour segmentation was not good enough: Io's lighting drifts over a session and the mask picks up
scenery, so a "rest" measurement taken minutes apart could differ by tens of pixels. Template
matching does not care about any of that. The gun is the highest-contrast object in its corner of
the frame, so normalised cross-correlation finds it again unambiguously, and the offset of the peak
is the answer in pixels.

Zero-mean normalised correlation is used rather than plain correlation so that a lighting change
between the two frames cannot masquerade as a shift.

  dx > 0  the weapon moved right on screen
  dy > 0  the weapon moved down on screen
  peak    correlation at the best offset; below about 0.5 the match is not trustworthy, which
          usually means the weapon left the search window or changed size a great deal

Usage: python weapon_shift.py <shots dir> <baseline.png> <shot.png> [more shots...]
"""

import sys
from pathlib import Path

import numpy as np
from PIL import Image

CLIENT = (9, 38, 9 + 1280, 38 + 720)
# Template box around the weapon at rest, in client pixels, and how far to search for it.
TEMPLATE = (830, 380, 1100, 700)
SEARCH = 200


def client(path):
    return np.asarray(Image.open(path).convert("L").crop(CLIENT), dtype=np.float32)


def scan(base, shot, box, centre, radius, step):
    """@return The best (dx, dy, score) within radius of centre, at the given step."""
    x0, y0, x1, y1 = box
    template = base[y0:y1, x0:x1]
    template = template - template.mean()
    norm = np.sqrt((template ** 2).sum())
    height, width = shot.shape
    best = (centre[0], centre[1], -1.0)
    for dy in range(centre[1] - radius, centre[1] + radius + 1, step):
        for dx in range(centre[0] - radius, centre[0] + radius + 1, step):
            sy0, sx0, sy1, sx1 = y0 + dy, x0 + dx, y1 + dy, x1 + dx
            if sy0 < 0 or sx0 < 0 or sy1 > height or sx1 > width:
                continue
            window = shot[sy0:sy1, sx0:sx1]
            centred = window - window.mean()
            denominator = norm * np.sqrt((centred ** 2).sum())
            if denominator <= 0.0:
                continue
            score = float((template * centred).sum() / denominator)
            if score > best[2]:
                best = (dx, dy, score)
    return best


def best_shift(base, shot):
    """@return dx, dy and the correlation peak of the template's best match in shot.

    Coarse to fine, because an exhaustive full-resolution search over the whole window costs tens
    of seconds per capture and there are dozens of captures per run. The coarse pass runs on
    quarter-scale images, which is both sixteen times cheaper per window and tolerant of the
    scene's fine-grained animation; the fine pass then refines to the pixel at full resolution.
    """
    quarter = tuple(v // 4 for v in TEMPLATE)
    small_base, small_shot = base[::4, ::4], shot[::4, ::4]
    coarse = scan(small_base, small_shot, quarter, (0, 0), SEARCH // 4, 1)
    centre = (coarse[0] * 4, coarse[1] * 4)
    return scan(base, shot, TEMPLATE, centre, 6, 1)


def main():
    directory = Path(sys.argv[1])
    base = client(directory / sys.argv[2])
    print(f"baseline: {sys.argv[2]}")
    print(f"{'shot':<28} {'dx':>6} {'dy':>6} {'peak':>7}")
    for name in sys.argv[3:]:
        dx, dy, peak = best_shift(base, client(directory / name))
        flag = "" if peak > 0.5 else "  (weak match)"
        print(f"{name:<28} {dx:>+6d} {dy:>+6d} {peak:>7.3f}{flag}")


if __name__ == "__main__":
    main()
