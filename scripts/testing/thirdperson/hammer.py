"""cap_hammer (Titan, Hammer of Sol start): what of the player's body survives our rules, and why.

Lists (a) every palette rooted at the player's feet or collapsed (bones, scale, header origin, prefix of the
gloves' skeleton, state), (b) the CPU cloth draws and the hidden header each would need, (c) every small
rigid object (<= 8 bones) within 3 m with its place in the root frame and the hand distances, and (d) every
draw with a 128-B world matrix within 2.5 m of the feet outside the G-buffer (effects), with its extent.

    set SVR_CAPTURES=...\\build\\shots\\super_hammer
    python hammer.py cap_hammer
"""
import sys
from collections import OrderedDict

import numpy as np

import body
import cap
import cloth

np.set_printoptions(precision=3, suppress=True, linewidth=160)


def main():
    name = sys.argv[1]
    draws, shaders, layouts, blobs = cap.load(name)
    cam = body.main_camera(draws, blobs)
    gloves = None
    for r in draws:
        f = cap.cbf(r, 11, blobs)
        if f is None or r['vcb'][11][3] < 1024:
            continue
        m = cap.rigid_bones(f)
        if len(m) >= 60:
            gloves = m
            break
    root = gloves[0]
    R, T = root[:, :3], root[:, 3]

    def in_root(p):
        return R.T @ (p - T)

    # game's palms: wrist bones 21/22 at their bind joints (project.py)
    palm = {s: gloves[b, :, :3] @ np.array(J) + gloves[b, :, 3] for s, b, J in
            (('L', 21, (0.292, 0.392, 1.156)), ('R', 22, (0.292, -0.392, 1.156)))}
    print(f'camera {cam} feet {T} camera-in-root {in_root(cam)}')
    print(f'game palms in root: L {in_root(palm["L"])} R {in_root(palm["R"])}')

    print('\n(a) palettes at the feet or collapsed')
    seen = OrderedDict()
    for r in draws:
        e = r['vcb'].get(11)
        f = cap.cbf(r, 11, blobs)
        if e is None or f is None or e[3] < 1024 or len(f) < 44:
            continue
        m = cap.rigid_bones(f)
        zero = np.abs(f[32:44]).max() == 0
        if not zero and (len(m) == 0 or np.linalg.norm(m[0, :, 3] - T) > 0.35):
            continue
        key = e[5]
        s = seen.setdefault(key, dict(w=e[3], scale=float(f[16]), origin=f[20:23].copy(), m=m, zero=zero, passes=OrderedDict(), vbs=set()))
        s['passes'][body.pass_label(r, blobs)] = s['passes'].get(body.pass_label(r, blobs), 0) + 1
        s['vbs'].add(r['vb'].get(0, ('-',))[0][-6:])
    for key, s in seen.items():
        m = s['m']
        if s['zero']:
            st = 'COLLAPSED'
            pre = '-'
        else:
            k = len(m)
            d = np.abs(m - gloves[:k]).max() if k <= len(gloves) else -1
            st = f'{k} bones LIVE'
            pre = f'max|bone-gloves|={d:.4f}'
        print(f"  b{key} cb{s['w']} scale={s['scale']:.4f} origin={s['origin']} {st} {pre} vbs={sorted(s['vbs'])} passes={dict(s['passes'])}")
        if not s['zero'] and len(m) <= 8:
            for i, b in enumerate(m):
                print(f'      bone {i} in root {in_root(b[:, 3])}')

    print('\n(c) small rigid objects (<= 8 bones) within 3 m')
    objs = OrderedDict()
    for r in draws:
        e = r['vcb'].get(11)
        f = cap.cbf(r, 11, blobs)
        if e is None or f is None or e[3] < 1024 or len(f) < 44:
            continue
        m = cap.rigid_bones(f)
        if not (0 < len(m) <= 8) or np.linalg.norm(m[0, :, 3] - T) > 3:
            continue
        o = objs.setdefault(e[5], dict(m=m, scale=float(f[16]), origin=f[20:23].copy(), passes=OrderedDict()))
        p = body.pass_label(r, blobs)
        o['passes'][p] = o['passes'].get(p, 0) + 1
    for key, o in objs.items():
        p = o['m'][0, :, 3]
        q = in_root(p)
        rad = np.hypot(q[0], q[1])
        inside = rad < 0.7 and 0.3 < q[2] < 2.1
        print(f"  b{key} bones={len(o['m'])} scale={o['scale']:.4f} origin={o['origin']} root-frame={q} radial={rad:.2f} "
              f"palmL={np.linalg.norm(p - palm['L']):.2f} palmR={np.linalg.norm(p - palm['R']):.2f} volume_rule={'IN' if inside else 'out'} passes={dict(o['passes'])}")

    print('\n(b) cloth (identity 128-B cb11, cloth layout) and the header each needs')
    ils = cloth.cloth_layouts(layouts)
    for r in draws:
        if r.get('il') not in ils:
            continue
        f = cap.cbf(r, 11, blobs)
        print(f"  i={r['i']} {body.pass_label(r, blobs)} n={r['n']} header scale={f[16]:.4f} origin={f[20:23]} vb off={r['vb'][0][2]}")

    print('\n(d) 128-B world-matrix draws within 2.5 m of the feet, not G-buffer')
    grp = OrderedDict()
    for r in draws:
        e = r['vcb'].get(11)
        f = cap.cbf(r, 11, blobs)
        if e is None or f is None or e[3] != 128:
            continue
        M = f[:16].reshape(4, 4)
        if np.allclose(M, np.eye(4), atol=1e-5):
            continue
        t = M[:3, 3] if abs(M[3, 3] - 1) < 1e-3 and np.abs(M[3, :3]).max() < 1e-3 else M[3, :3]
        if np.linalg.norm(t - T) > 2.5:
            continue
        p = body.pass_label(r, blobs)
        sc = np.linalg.norm(M[:3, :3], axis=0) if abs(M[3, 3] - 1) < 1e-3 and np.abs(M[3, :3]).max() < 1e-3 else np.linalg.norm(M[:3, :3], axis=1)
        k = (r.get('vsh'), r.get('psh'), tuple(np.round(in_root(t), 2)))
        g = grp.setdefault(k, dict(ps=[], n=r['n'], inst=r['inst'], sc=sc, lay=layouts.get(r.get('il'), '-')[:60], vb=r['vb'].get(0, ('-', 0))[:2]))
        g['ps'].append(f"{r['i']}:{p}")
    for (vsh, psh, q), g in grp.items():
        print(f"  vsh={vsh} psh={psh} at-root-frame={q} axes-scale={np.round(g['sc'], 3)} n={g['n']} inst={g['inst']} vb={g['vb'][0][-6:]}:{g['vb'][1]} "
              f"draws={g['ps'][:6]}{' ...' if len(g['ps']) > 6 else ''}")


if __name__ == '__main__':
    main()
