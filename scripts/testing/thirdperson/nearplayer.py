"""Every draw (G-buffer, shadow cascades, forward) whose per-object cb11 puts it near the player, whatever its
form, plus the palettes our collapse zeroed, so what still draws at the body can be told from what we hid.

cb11 forms seen in this build:
  5120 B  armour palette   : rows 0-3 ?, header rows 4-5 (scale f[16], origin f[20:23]), 3x4 bones from row 8
  1024 B  weapon/object pal: same header, 3x4 bones from row 8
   128 B  world-space mesh : rows 0-3 a 4x4 (identity for CPU-written geometry), rows 4-5 the same header
    48 B  ?

    python nearplayer.py cap_cloth [--radius 2.5]
"""
import sys
from collections import OrderedDict

import numpy as np

import body
import cap


def classify(r, blobs):
    e = r['vcb'].get(11)
    f = cap.cbf(r, 11, blobs)
    if e is None or f is None:
        return None
    w = e[3]
    if w >= 1024 and len(f) >= 44:
        bones = f[32:]
        n = len(bones) // 12
        m = bones[:n * 12].reshape(n, 3, 4)
        rig = cap.rigid_bones(f)
        if len(rig):
            return dict(kind=f'pal{w}', bones=len(rig), root=rig[0, :, 3], scale=float(f[16]), origin=f[20:23].copy())
        if n and np.abs(m[0]).max() == 0:
            # collapsed: count leading all-zero bones
            k = 0
            while k < n and np.abs(m[k]).max() == 0:
                k += 1
            return dict(kind=f'zero{w}', bones=k, root=None, scale=float(f[16]), origin=f[20:23].copy())
        return dict(kind=f'other{w}', bones=0, root=None, scale=float(f[16]), origin=f[20:23].copy())
    if w == 128:
        M = f[:16].reshape(4, 4)
        return dict(kind='m128', bones=0, M=M, root=M[:3, 3] if abs(M[3, 3] - 1) < 1e-3 and np.abs(M[3, :3]).max() < 1e-3 else M[3, :3],
                    scale=float(f[16]), origin=f[20:23].copy())
    return dict(kind=f'w{w}', bones=0, root=None, scale=float(f[16]) if len(f) > 16 else 0, origin=None)


def main():
    name = sys.argv[1]
    radius = float(sys.argv[sys.argv.index('--radius') + 1]) if '--radius' in sys.argv else 2.5
    draws, shaders, layouts, blobs = cap.load(name)
    cam = body.main_camera(draws, blobs)
    # the player's root = the gloves' (72-bone) root
    feet = None
    for r in draws:
        c = classify(r, blobs)
        if c and c['kind'].startswith('pal') and c['bones'] >= 52:
            feet = c['root']
            break
    print(f'{name}: camera {cam} feet {feet}')
    g = OrderedDict()
    for r in draws:
        c = classify(r, blobs)
        if c is None:
            continue
        near = c['root'] is not None and np.linalg.norm(c['root'] - feet) < radius
        if not (near or c['kind'].startswith('zero')):
            continue
        if c['kind'] == 'm128' and np.allclose(c['M'], np.eye(4), atol=1e-5):
            tag = 'm128-identity'
        else:
            tag = c['kind']
        key = (tag, r['vcb'][11][5] if tag != 'm128-identity' else 'id', r['vb'].get(0, ('-',))[0], round(c['scale'], 4))
        e = g.setdefault(key, dict(c=c, draws=[], vsh=set(), lay=layouts.get(r.get('il'), '-')))
        e['draws'].append((r['i'], body.pass_label(r, blobs), r['n'], r.get('si', '0'), r['vb'].get(0, ('-', 0, 0, 0, '-'))[2:5]))
        e['vsh'].add(r.get('vsh'))
    for (tag, bid, vb, sc), e in g.items():
        c = e['c']
        rs = '-' if c['root'] is None else '(' + ','.join(f'{v:.3f}' for v in c['root']) + ')'
        orig = '-' if c['origin'] is None else '(' + ','.join(f'{v:.4f}' for v in c['origin']) + ')'
        print(f'{tag:14s} b{bid} vb={vb[-8:]} bones={c["bones"]} scale={sc} origin={orig} root={rs} vsh={sorted(e["vsh"])}')
        print(f'      layout {e["lay"][:110]}')
        print('      ' + ' '.join(f'{i}:{p}:n{n}:si{si}' for i, p, n, si, _ in e['draws'][:14]) + (' ...' if len(e['draws']) > 14 else ''))


if __name__ == '__main__':
    main()
