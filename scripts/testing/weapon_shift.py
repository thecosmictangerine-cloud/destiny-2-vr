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


def _template_fraction():
    """@return The template box as fractions of the client area (left, top, right, bottom).

    Overridable through `SVR_TEMPLATE_BOX` as four comma-separated fractions, because a box that
    does not contain the weapon is the nastiest failure this tool has: the template is then just
    scenery, scenery does not move, and the answer comes back as a confident 0 px shift at a
    correlation peak of up to 0.97. Three points of one pivot scan were false zeros exactly that
    way. The default suits the engine's own viewmodel position; the absolute placement moves the
    weapon somewhere else, so the tests that move it must say where to look.
    """
    import os
    import re
    raw = os.environ.get("SVR_TEMPLATE_BOX", "")
    match = re.match(r"^\s*([\d.]+),([\d.]+),([\d.]+),([\d.]+)\s*$", raw)
    if match:
        left, top, right, bottom = (float(g) for g in match.groups())
        if 0.0 <= left < right <= 1.0 and 0.0 <= top < bottom <= 1.0:
            return (left, top, right, bottom)
    return (0.648, 0.528, 0.859, 0.972)


CLIENT = _client_rect()
# Template box around the weapon, and how far to search for it, both as fractions of the client
# area so they follow the window size instead of being tied to one resolution.
TEMPLATE_FRACTION = _template_fraction()
# Wide enough for the amplitudes the tests actually use: 0.1 m of travel with the weapon 0.33 m
# from the eye is about 17 degrees, and at 104 degrees across 1920 pixels that is over 300 px.
SEARCH_FRACTION = 0.32


def client(path):
    return np.asarray(Image.open(path).convert("L").crop(CLIENT), dtype=np.float32)


def rotated_template(base, box, degrees):
    """@return The template taken from base's box, rotated by `degrees` in the image plane.

    Needed because normalised cross-correlation is NOT rotation invariant, and the measurement that
    matters most -- how far a wrist ROLL translates the weapon -- rotates the gun's image by the
    roll angle. Matching an unrotated template against a rolled gun collapsed the peak to 0.04 and
    returned a spurious offset 582 px away, which is a measurement that looks like a number and is
    not one.

    The rotation is taken from a region LARGER than the box and then centre-cropped back, so the
    template is filled with real pixels instead of the black corners a naive rotate would leave --
    black corners correlate with dark scenery and would bias the peak.
    """
    x0, y0, x1, y1 = box
    if abs(degrees) < 1e-6:
        return base[y0:y1, x0:x1]
    width, height = x1 - x0, y1 - y0
    centre = ((x0 + x1) // 2, (y0 + y1) // 2)
    side = int(1.6 * max(width, height))
    half = side // 2
    top, left = centre[1] - half, centre[0] - half
    if top < 0 or left < 0 or top + side > base.shape[0] or left + side > base.shape[1]:
        return base[y0:y1, x0:x1]
    patch = Image.fromarray(base[top:top + side, left:left + side])
    patch = patch.rotate(degrees, resample=Image.BICUBIC)
    rotated = np.asarray(patch, dtype=np.float32)
    cy, cx = side // 2, side // 2
    return rotated[cy - height // 2:cy - height // 2 + height, cx - width // 2:cx - width // 2 + width]


def template_box(width, height):
    """@return The template box in pixels for a client area of this size."""
    left, top, right, bottom = TEMPLATE_FRACTION
    return (int(left * width), int(top * height), int(right * width), int(bottom * height))


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


def scan_template(template, shot, box, centre, radius, step):
    """@return The best (dx, dy, score) of an already-extracted template within radius of centre."""
    x0, y0, x1, y1 = box
    centred_template = template - template.mean()
    norm = np.sqrt((centred_template ** 2).sum())
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
            score = float((centred_template * centred).sum() / denominator)
            if score > best[2]:
                best = (dx, dy, score)
    return best


def best_shift(base, shot, rotate=0.0):
    """@return dx, dy, the correlation peak, and the template rotation that won.

    Coarse to fine, because an exhaustive full-resolution search over the whole window costs tens
    of seconds per capture and there are dozens of captures per run. The coarse pass runs on
    quarter-scale images, which is both sixteen times cheaper per window and tolerant of the
    scene's fine-grained animation; the fine pass then refines to the pixel at full resolution.

    When `rotate` is non-zero the whole search is repeated for template rotations of 0 and plus and
    minus that angle, and the best peak wins. Trying both signs rather than deriving one is
    deliberate: the sign depends on how the controller's roll axis maps onto the image plane, and
    letting the correlation decide is both cheaper and more honest than asserting it.
    """
    height, width = base.shape
    box = template_box(width, height)
    search = int(SEARCH_FRACTION * width)
    quarter = tuple(v // 4 for v in box)
    small_base, small_shot = base[::4, ::4], shot[::4, ::4]
    angles = [0.0] if abs(rotate) < 1e-6 else [0.0, rotate, -rotate]
    best = None
    for angle in angles:
        small_template = rotated_template(small_base, quarter, angle)
        coarse = scan_template(small_template, small_shot, quarter, (0, 0), search // 4, 1)
        centre = (coarse[0] * 4, coarse[1] * 4)
        template = rotated_template(base, box, angle)
        fine = scan_template(template, shot, box, centre, 6, 1)
        if best is None or fine[2] > best[2]:
            best = (fine[0], fine[1], fine[2], angle)
    return best


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    rotate = 0.0
    for a in sys.argv[1:]:
        if a.startswith("--rotate"):
            rotate = float(a.split("=", 1)[1]) if "=" in a else 0.0
    directory = Path(args[0])
    base = client(directory / args[1])
    print(f"baseline: {args[1]}  rotate: {rotate}")
    print(f"{'shot':<28} {'dx':>6} {'dy':>6} {'peak':>7} {'rot':>6}")
    for name in args[2:]:
        dx, dy, peak, angle = best_shift(base, client(directory / name), rotate)
        flag = "" if peak > 0.5 else "  (weak match)"
        print(f"{name:<28} {dx:>+6d} {dy:>+6d} {peak:>7.3f} {angle:>+6.1f}{flag}")


if __name__ == "__main__":
    main()
