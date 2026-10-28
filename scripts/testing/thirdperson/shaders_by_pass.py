"""Vertex shaders used by the player's own draws, per pass (player = palette root within 4 m of the
main camera, VS that reads a 4-component POSITION, i.e. a palette reader).

    python shaders_by_pass.py cap_sw
"""
import sys
from collections import Counter, defaultdict

import numpy as np

import body
import cap


def main():
    name = sys.argv[1]
    draws, shaders, layouts, blobs = cap.load(name)
    cam = body.main_camera(draws, blobs)
    out = defaultdict(Counter)
    for r in draws:
        sig = shaders.get(r.get('vs'), ('', ''))[1]
        if 'POSITION0:F' not in sig:
            continue
        f = cap.cbf(r, 11, blobs)
        if f is None or len(f) < 44:
            continue
        m = cap.rigid_bones(f)
        if len(m) == 0:
            continue
        v = cap.view(r, blobs)
        vm = v is not None and np.abs(v[7, :3]).max() < 1e-3
        if not vm and np.linalg.norm(m[0, :, 3] - cam) > 4:
            continue
        skinned = 'BLENDINDICES' in sig
        out[body.pass_label(r, blobs)][(r.get('vsh'), 'skin' if skinned else 'rigid', len(m) if len(m) in (1, 4, 5, 8) else 'body')] += 1
    for p, c in out.items():
        print(p, dict(c))


if __name__ == '__main__':
    main()
