"""Which bone of the posed arm palettes (gloves 72, gauntlets 62) could put mesh vertices on the arcs: the arc points
(arcs.py, saved as arcs_world.npy in the working directory) taken back into each bone's bind space,
v_bind = R_b^T (W - t_b). A bone whose back-mapped arc lies on the Guardian's bind-pose body (|x| < 0.3, |y| < 0.6,
0 < z < 2) is one that can draw there; bones whose matrix is the root's (identity rotation) map the arcs to where they are.
Also: each bone's rotation angle against the root and its translation in the root frame.

    python arcbones.py cap_arcs <dir with arcs_world.npy>
"""
import os
import sys

import numpy as np

import cap

np.set_printoptions(precision=3, suppress=True, linewidth=170)


def angle(R):
    return np.degrees(np.arccos(np.clip((np.trace(R) - 1) / 2, -1, 1)))


def main():
    name, d = sys.argv[1], sys.argv[2]
    W = np.load(os.path.join(d, 'arcs_world.npy'))
    draws, shaders, layouts, blobs = cap.load(name)
    pals = {}
    for r in draws:
        e = r['vcb'].get(11)
        f = cap.cbf(r, 11, blobs)
        if e is None or f is None or e[3] < 1024:
            continue
        m = cap.rigid_bones(f)
        if len(m) >= 52 and e[5] not in pals:
            pals[e[5]] = (m, float(f[16]), f[20:23].copy())
    root = next(iter(pals.values()))[0][0]
    R0, T0 = root[:, :3], root[:, 3]
    for bid, (m, sc, org) in pals.items():
        print(f'== b{bid}: {len(m)} bones, header scale {sc:.4f} origin {org}')
        for i, bn in enumerate(m):
            Rb, tb = bn[:, :3], bn[:, 3]
            Rrel = R0.T @ Rb
            vb = (W - tb) @ Rb          # = Rb^T (W - tb) per row
            lo, hi = vb.min(0), vb.max(0)
            onbody = abs(lo[0]) < 0.4 and abs(hi[0]) < 0.4 and abs(lo[1]) < 0.7 and abs(hi[1]) < 0.7 and lo[2] > 0 and hi[2] < 2.0
            print(f'  bone {i:2d} rot {angle(Rrel):6.1f} deg  t_root {((tb - T0) @ R0)}  arcs in bind: '
                  f'x {lo[0]:6.2f}..{hi[0]:6.2f} y {lo[1]:6.2f}..{hi[1]:6.2f} z {lo[2]:6.2f}..{hi[2]:6.2f}{"  <== ON BODY" if onbody else ""}')


if __name__ == '__main__':
    main()
