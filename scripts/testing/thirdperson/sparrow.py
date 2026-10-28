"""cap_sp: the rider on the sparrow.

Rider: 72-bone family near the camera; root frame, head/hand joints (bind joints solved across
cap_sw + cap_1p poses + this one), camera offsets. Sparrow: the non-body palettes rooted near
the rider; every bone's transform, its relation to the rider root/head, draws, passes, shaders.

    python sparrow.py
"""
import numpy as np

import body
import cap
import skel


def yaw_pitch_roll(R):
    f = R[:, 0]
    yaw = np.degrees(np.arctan2(f[1], f[0]))
    pitch = np.degrees(np.arcsin(np.clip(f[2], -1, 1)))
    left = R[:, 1]
    roll = np.degrees(np.arcsin(np.clip(left[2], -1, 1)))
    return yaw, pitch, roll


def main():
    np.set_printoptions(precision=4, suppress=True)
    cam, g, draws, blobs = body.groups('cap_sp', 5.0)
    _, pal_sw, _, _ = skel.palettes('cap_sw')
    _, pal_1p, _, _ = skel.palettes('cap_1p')
    s72 = max(pal_sw.values(), key=lambda e: e['bones'])['pal']
    p72 = max(pal_1p.values(), key=lambda e: e['bones'])['pal']
    bodyp = {}
    other = {}
    for (vb, bid), e in g.items():
        (bodyp if e['bones'] in (19, 27, 29, 52, 72) and abs(e['pal'][0, 2, 3] - 0) >= 0 and e['scale'] in
         (e['scale'],) and e['bones'] >= 19 else other).setdefault(bid, e)
    a72 = max(bodyp.values(), key=lambda e: e['bones'])['pal']
    print('main camera', cam)
    for bid, e in bodyp.items():
        k = min(e['bones'], 72)
        print(f"  rider b{bid} bones={e['bones']} s={e['scale']:.4f} prefix_diff={np.abs(e['pal'][:k]-a72[:k]).max():.1e} passes={dict(e['passes'])} vsh={dict(e['vsh'])}")
    R0, t0 = a72[0, :, :3], a72[0, :, 3]
    print('rider bone0 R=\n', R0, '\n t=', t0, ' yaw/pitch/roll(model X fwd, Y left)=', yaw_pitch_roll(R0))
    poses = [s72, p72, a72]
    pts = {}
    for lab, (p, c) in {'head pivot 16-18': (16, 18), 'neck 13-18': (13, 18), 'wrist L 19-21': (19, 21),
                        'wrist R 20-22': (20, 22), 'shoulder L 12-15': (12, 15), 'shoulder R 14-17': (14, 17),
                        'pelvis-spine 1-5': (1, 5), 'hip L 1-3': (1, 3), 'hip R 1-4': (1, 4),
                        'ankle L 6-9': (6, 9), 'ankle R 7-10': (7, 10)}.items():
        J, res, sv = skel.solve_joint(poses, p, c)
        w = a72[c, :, :3] @ J + a72[c, :, 3]
        pts[lab] = w
        print(f'  {lab:18s} J={J} res={res*1000:.2f}mm world={w} rootframe={R0.T @ (w - t0)}')
    # palm points: bind wrist + 8 cm toward the fingers (bind +X) ~ grip
    for lab, c, palm in (('palm L', 21, (0.37, 0.40, 1.12)), ('palm R', 22, (0.37, -0.40, 1.12))):
        w = a72[c, :, :3] @ np.array(palm) + a72[c, :, 3]
        pts[lab] = w
        print(f'  {lab} (bind {palm}) world={w} rootframe={R0.T @ (w - t0)}')
    head = pts['head pivot 16-18']
    print('camera - root  world', cam - t0, ' rider frame', R0.T @ (cam - t0))
    print('camera - head  world', cam - head, ' rider frame', R0.T @ (cam - head))
    for r in draws:
        v = cap.view(r, blobs)
        if v is not None and body.view_kind(v) == 'main':
            fwd = -v[6, :3]
            print('camera forward (-row6)', fwd, 'heading', np.degrees(np.arctan2(fwd[1], fwd[0])), 'pitch', np.degrees(np.arcsin(fwd[2])))
            break
    print('\n== other palettes near the rider')
    seen = set()
    for (vb, bid), e in g.items():
        if bid in bodyp:
            continue
        m = e['pal']
        if bid not in seen:
            seen.add(bid)
            r0 = next(x for x in draws if x['i'] == e['draws'][0])
            print(f"-- b{bid} bones={e['bones']} s={e['scale']:.4f} origin={e['origin']} cb11={r0['vcb'][11][0][-10:]} bw={r0['vcb'][11][3]}")
            for k in range(e['bones']):
                Rk, tk = m[k, :, :3], m[k, :, 3]
                print(f'    bone{k} t={tk} ypr={np.round(yaw_pitch_roll(Rk), 2)} R=\n', Rk)
        sig = None
        print(f"   vb={vb[-8:]} stride={e['stride']} vbw={e['vbw']} draws={e['draws']} idx={dict(e['n'])} vsh={dict(e['vsh'])} passes={dict(e['passes'])}")
    # sparrow frame: bone 0 of the 10-bone palette (most bones)
    sp = max((e for b, e in g.items() if b[1] not in bodyp), key=lambda e: e['bones'])['pal']
    S, st = sp[0, :, :3], sp[0, :, 3]
    print('\nsparrow bone0 t', st, 'ypr', yaw_pitch_roll(S))
    print('rider root in sparrow frame', S.T @ (t0 - st))
    print('rider head in sparrow frame', S.T @ (head - st))
    print('palm L/R in sparrow frame', S.T @ (pts['palm L'] - st), S.T @ (pts['palm R'] - st))
    print('camera in sparrow frame', S.T @ (cam - st))
    print('rider R0 vs sparrow S relative rotation:\n', S.T @ R0)


if __name__ == '__main__':
    main()
