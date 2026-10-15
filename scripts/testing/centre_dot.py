"""Counts the ADS centre dot's pixels: saturated red with a white core, at the middle of the frame.

    python centre_dot.py <capture.png> [--box 40]

Prints the number of strongly red pixels (R high, G and B low) within +-box px of the centre. The dot
is a few hundred such pixels at 1080p; the scene there is grass and sand, which has none.
"""
import sys

import numpy as np
from PIL import Image


def main():
    path = sys.argv[1]
    box = int(sys.argv[sys.argv.index('--box') + 1]) if '--box' in sys.argv else 40
    im = np.asarray(Image.open(path).convert('RGB')).astype(int)
    h, w, _ = im.shape
    c = im[h // 2 - box:h // 2 + box, w // 2 - box:w // 2 + box]
    r, g, b = c[..., 0], c[..., 1], c[..., 2]
    red = (r > 170) & (g < 110) & (b < 110)
    print(int(red.sum()))


if __name__ == '__main__':
    main()
