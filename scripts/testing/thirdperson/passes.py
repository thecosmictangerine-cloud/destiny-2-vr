"""Timeline of passes: contiguous runs of draws sharing render target set and cb12 view.

For each run: draw range, target tag (format codes; +ds depth bound; nort = depth only),
viewport, the view's camera position, P00/P11/P22/P32 and the eye point recovered from
the view-projection (rows 0-3, transposed) by solving VP^T * (x,1) = (0,0,*,0).

    python passes.py cap_sw
"""
import sys

import numpy as np

import cap


def eye_from_vp(v):
    VP = v[0:4]  # row r = input component r (x,y,z,1), column c = clip component
    M = VP.T     # clip = M @ (x,y,z,1)
    # Camera centre: point where clip x, y, w all vanish (perspective) -> null space of rows 0,1,3
    A = M[[0, 1, 3], :]
    try:
        _, s, vt = np.linalg.svd(A)
        c = vt[-1]
        if abs(c[3]) < 1e-9:
            return None, 'ortho'
        return c[:3] / c[3], 'persp'
    except Exception:
        return None, '?'


def main():
    name = sys.argv[1]
    draws, shaders, layouts, blobs = cap.load(name)
    runs = []
    for r in draws:
        cb = r['vcb'].get(12)
        key = (cap.rt_tag(r), r.get('rt', '-'), r.get('ds', '-'), cb[5] if cb else None, r.get('vp'))
        if runs and runs[-1][0] == key:
            runs[-1][2] = r['i']
            runs[-1][3] += 1
        else:
            runs.append([key, r['i'], r['i'], 1])
    for key, a, b, n in runs:
        tag, rt, ds, bid, vp = key
        desc = ''
        if bid is not None:
            f = np.frombuffer(blobs[bid][:224], '<f4') if len(blobs.get(bid, b'')) >= 224 else None
            if f is not None:
                v = f.reshape(14, 4)
                e, kind = eye_from_vp(v)
                es = f'({e[0]:.2f},{e[1]:.2f},{e[2]:.2f})' if e is not None else kind
                desc = (f'b{bid} cam=({v[7,0]:.2f},{v[7,1]:.2f},{v[7,2]:.2f}) P00={v[10,0]:.3f} P11={v[11,1]:.3f} '
                        f'P22={v[12,2]:.3f} P32={v[13,2]:.4f} eyeVP={es}')
        print(f'{a:5d}-{b:5d} n={n:4d} {tag:14s} rt={rt[-6:]} ds={ds[-6:]} vp={vp} {desc}')


if __name__ == '__main__':
    main()
