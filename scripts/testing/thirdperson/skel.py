"""Q2/Q5: the body skeleton.

1. Prefix test: are all the player's palettes the first N bones of one skeleton (same matrices)?
2. Root frame (bone 0) and character yaw; main camera's view rows.
3. Joint solve across poses: for bone pairs (p, c) a shared joint J (bind space) satisfies
   (R_p - R_c) J = t_c - t_p in every pose. Stacking poses from several palettes of the SAME
   skeleton (3P body in cap_sw, 3P body in cap_1p's shadow pass, the 1P viewmodel arms in
   cap_1p, which live in camera-relative space -- the equation is frame-invariant) gives an
   over-determined system whose residual says whether the pair really shares a joint.
   If the 3P skeleton is the 1P skeleton, the 1P joints of docs/ARMS.md come back.

    python skel.py
"""
import numpy as np

import cap
import body


def palettes(name):
    cam, g, draws, blobs = body.groups(name, 4.0)
    out = {}
    for (vb, bid), e in g.items():
        out.setdefault(bid, e)
    return cam, out, draws, blobs


def vm_palettes(name):
    draws, shaders, layouts, blobs = cap.load(name)
    out = {}
    for r in draws:
        v = cap.view(r, blobs)
        if v is None or np.abs(v[7, :3]).max() > 1e-3 or abs(v[12, 2]) < 1e-4:
            continue
        f = cap.cbf(r, 11, blobs)
        if f is None:
            continue
        m = cap.rigid_bones(f)
        if len(m):
            out.setdefault(r['vcb'][11][5], dict(bones=len(m), pal=m, scale=float(f[16]), origin=f[20:23].copy(),
                                                 vb=r['vb'].get(0, ('-',))[0]))
    return out


def solve_joint(poses, p, c):
    A, b = [], []
    for m in poses:
        A.append(m[p, :, :3] - m[c, :, :3])
        b.append(m[c, :, 3] - m[p, :, 3])
    A = np.vstack(A)
    b = np.concatenate(b)
    J, *_ = np.linalg.lstsq(A, b, rcond=None)
    res = np.abs(A @ J - b).max()
    return J, res, np.linalg.svd(A, compute_uv=False)


def yaw_deg(R):
    # R maps bind (model) axes to world; model +X is the character's forward.
    f = R[:, 0]
    return np.degrees(np.arctan2(f[1], f[0]))


def main():
    np.set_printoptions(precision=4, suppress=True)
    caps = {}
    for name in ('cap_sw', 'cap_1p'):
        cam, pals, draws, blobs = palettes(name)
        caps[name] = (cam, pals)
        print(f'== {name}: main camera {cam}')
        big = [e for e in pals.values() if e['bones'] >= 19]
        skel = max(big, key=lambda e: e['bones'])['pal'] if big else None
        for bid, e in pals.items():
            n = e['bones']
            if skel is not None and n >= 2:
                k = min(n, len(skel))
                d = np.abs(e['pal'][:k] - skel[:k]).max()
            else:
                d = float('nan')
            print(f'  b{bid:<5} bones={n:<3} s={e["scale"]:.4f} origin={e["origin"]} maxdiff_vs_72prefix={d:.2e}')
        if skel is not None:
            R0, t0 = skel[0, :, :3], skel[0, :, 3]
            print('  bone0 R=\n', R0, '\n  t=', t0, f' yaw(model +X)={yaw_deg(R0):.1f} deg; det={np.linalg.det(R0):.4f}')
            print('  camera - root =', cam - t0, ' in root frame:', R0.T @ (cam - t0))
            # where each bone maps the bind-space model origin and a 1.7 m point
            print('  per-bone M*(0,0,0) z range', skel[:, 2, 3].min(), skel[:, 2, 3].max())
        # main view rows
        draws2, _, _, blobs2 = cap.load(name)
        for r in draws2:
            v = cap.view(r, blobs2)
            if v is not None and body.view_kind(v) == 'main':
                print('  view rows4-6:\n', v[4:7, :3])
                f = v[4, :3]
                print(f'  view row4 heading {np.degrees(np.arctan2(f[1], f[0])):.1f} deg')
                for row in (4, 5, 6):
                    print(f'   row{row} heading {np.degrees(np.arctan2(v[row,1], v[row,0])):.1f} pitch {np.degrees(np.arcsin(np.clip(v[row,2],-1,1))):.1f}')
                break

    vm = vm_palettes('cap_1p')
    print('== 1P viewmodel palettes:', {k: (e['bones'], round(e['scale'], 4)) for k, e in vm.items()})
    p72_sw = max(caps['cap_sw'][1].values(), key=lambda e: e['bones'])['pal']
    p72_1p = max(caps['cap_1p'][1].values(), key=lambda e: e['bones'])['pal']
    vm72 = max(vm.values(), key=lambda e: e['bones'])['pal']
    print('bone counts:', len(p72_sw), len(p72_1p), len(vm72))
    print('3P sw vs 3P 1p palette max diff (same pose?):', np.abs(p72_sw - p72_1p).max())
    poses_all = [p72_sw, p72_1p, vm72]
    known = {'shoulder L (12,15)': (12, 15), 'shoulder R (14,17)': (14, 17), 'elbow L (15,19)': (15, 19),
             'elbow R (17,20)': (17, 20), 'wrist L (19,21)': (19, 21), 'wrist R (20,22)': (20, 22)}
    print('\nARMS.md 1P joints: shoulder (-0.027,+-0.186,1.485) elbow (0.077,+-0.324,1.274) wrist (0.292,+-0.392,1.156)')
    for label, (p, c) in known.items():
        for tag, poses in (('3P sw+3P 1p', [p72_sw, p72_1p]), ('3P sw+1P vm', [p72_sw, vm72]),
                           ('all three', poses_all)):
            J, res, sv = solve_joint(poses, p, c)
            print(f'  {label:20s} {tag:12s} J={J} res={res:.4f} sv={sv}')

    # full joint table for the first 29 bones: best partner (parent guess) per bone
    print('\nPair joints over all three poses (res < 3 mm, J inside a body box):')
    nb = 29
    rows = []
    for c in range(1, nb):
        best = None
        for p in range(0, nb):
            if p == c:
                continue
            J, res, sv = solve_joint(poses_all, p, c)
            if sv[-1] < 1e-3:
                continue  # rigidly together in every pose: joint undetermined
            if res < 0.003 and np.all(np.abs(J[:2]) < 1.0) and -0.1 < J[2] < 2.1:
                rows.append((c, p, J, res))
    for c, p, J, res in rows:
        print(f'  {p:2d}-{c:2d} J=({J[0]: .3f},{J[1]: .3f},{J[2]: .3f}) res={res*1000:.2f} mm')

    # world positions of a few joints in cap_sw
    print('\nWorld points in cap_sw (M_bone * J):')
    for label, (p, c) in list(known.items()):
        J, res, sv = solve_joint(poses_all, p, c)
        w = p72_sw[c, :, :3] @ J + p72_sw[c, :, 3]
        print(f'  {label:20s} world={w}')


if __name__ == '__main__':
    main()
