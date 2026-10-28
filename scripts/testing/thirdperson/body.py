"""Q2/Q4: skinned draws whose (absolute) palette sits near the main world camera.

Groups by (vertex buffer, palette blob); for each: bone count, dequant scale/origin, root
translation, vsh set, passes (labelled from the cb12 view + render targets).

    python body.py cap_sw [--radius 4]
"""
import sys
from collections import Counter, OrderedDict

import numpy as np

import cap

MAIN = {}


def view_kind(v):
    if v is None:
        return 'noview'
    if np.abs(v[7, :3]).max() < 1e-3:
        return 'VM' if abs(v[12, 2]) > 1e-4 else 'zero'
    if abs(v[13, 3] - 1) < 1e-3 or (abs(v[12, 3]) < 1e-6 and abs(v[13, 3]) > 0.5):
        return 'ortho'
    if abs(v[10, 0] - 1) < 1e-3 and abs(v[11, 1] - 1) < 1e-3:
        return 'cube90'
    if abs(v[12, 2]) > 1e-4:
        return 'spot'
    return 'main'


def pass_label(r, blobs):
    v = cap.view(r, blobs)
    k = view_kind(v)
    tag = cap.rt_tag(r)
    vp = r.get('vp', '').split(',')
    if k == 'ortho':
        return f'CSM{vp[2]}'
    if k in ('main', 'VM'):
        if tag == 'nort+ds':
            return k + ':depth'
        if tag.startswith('f29+rt1'):
            return k + ':gbuf'
        if tag.startswith('f26'):
            return k + ':fwd' + tag[3:]
        return k + ':' + tag
    return k + ':' + tag


def main_camera(draws, blobs):
    c = Counter()
    for r in draws:
        v = cap.view(r, blobs)
        if v is not None and view_kind(v) == 'main':
            c[tuple(np.round(v[7, :3], 3))] += 1
    return np.array(c.most_common(1)[0][0])


def groups(name, radius=4.0, maxbones=None):
    draws, shaders, layouts, blobs = cap.load(name)
    cam = main_camera(draws, blobs)
    g = OrderedDict()
    for r in draws:
        f = cap.cbf(r, 11, blobs)
        if f is None or len(f) < 44:
            continue
        m = cap.rigid_bones(f)
        if len(m) == 0:
            continue
        root = m[0, :, 3]
        if np.linalg.norm(root - cam) > radius:
            continue
        key = (r['vb'].get(0, ('-',))[0], r['vcb'][11][5])
        e = g.setdefault(key, dict(bones=len(m), scale=float(f[16]), origin=f[20:23].copy(), pal=m, draws=[],
                                   vsh=Counter(), passes=Counter(), n=Counter(), stride=r['vb'].get(0, ('-', 0))[1],
                                   vbw=r['vb'].get(0, ('-', 0, 0, 0))[3]))
        e['draws'].append(r['i'])
        e['vsh'][r.get('vsh')] += 1
        e['passes'][pass_label(r, blobs)] += 1
        e['n'][r['n']] += 1
    return cam, g, draws, blobs


def main():
    name = sys.argv[1]
    radius = float(sys.argv[sys.argv.index('--radius') + 1]) if '--radius' in sys.argv else 4.0
    cam, g, draws, blobs = groups(name, radius)
    print(f'{name}: main camera {cam}')
    for (vb, bid), e in g.items():
        t = e['pal'][:, :, 3]
        print(f"vb={vb[-8:]} stride={e['stride']} vbw={e['vbw']} pal=b{bid} bones={e['bones']} s={e['scale']:.4f} "
              f"origin=({e['origin'][0]:.3f},{e['origin'][1]:.3f},{e['origin'][2]:.3f}) root=({t[0,0]:.3f},{t[0,1]:.3f},{t[0,2]:.3f}) "
              f"draws={len(e['draws'])} i={e['draws'][:8]} idxcount={dict(e['n'])} vsh={dict(e['vsh'])} passes={dict(e['passes'])}")


if __name__ == '__main__':
    main()
