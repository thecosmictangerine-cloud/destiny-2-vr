"""cap_arcs (Titan, sword, render camera at the head 65 deg down): where the two thin dark arcs left in the composed
eye image lie on the ground, and what is drawn there.

  1. eye pixels -> rays (inverse of project.py to_eye) -> the ground plane at the feet: the arcs in world / root frame
  2. every draw's per-object position (cb11 palette root, 128-B matrix translation) near those points, any pass
  3. for sun shadows use project.py (joints); a palette bone's translation is not its joint

    python arcs.py cap_arcs <eye0.bmp>
"""
import sys

import numpy as np
from PIL import Image

import body
import cap
import project

np.set_printoptions(precision=3, suppress=True, linewidth=170)


def eye_to_ground(v, px, py, ground):
    E = project.EYE
    l, r, u, d = (np.tan(np.radians(E[k])) for k in 'lrud')
    tx = l + px / E['w'] * (r - l)
    ty = u - py / E['h'] * (u - d)
    nx, ny = tx * v[10, 0], ty * v[11, 1]
    M = v[0:4].T
    Mi = np.linalg.inv(M)
    pts = []
    for z in (0.1, 0.9):
        h = Mi @ np.array([nx, ny, z, 1.0])
        pts.append(h[:3] / h[3])
    a, b = pts
    t = (ground - a[2]) / (b[2] - a[2])
    return a + t * (b - a)


def dark_pixels(img):
    im = np.asarray(Image.open(img).convert('RGB')).astype(int)
    lum = im.sum(axis=2)
    ys, xs = np.nonzero(lum < 200)
    # inside the world rectangle (not the letterbox: 0,0,70 sums to 70 -- exclude pure letterbox colour)
    keep = [(x, y) for x, y in zip(xs, ys) if not (im[y, x, 0] == 0 and im[y, x, 1] == 0)]
    return keep, im


def main():
    name, img = sys.argv[1], sys.argv[2]
    draws, shaders, layouts, blobs = cap.load(name)
    v = project.main_view(draws, blobs)
    L = project.csm_light_dir(draws, blobs)
    gloves = None
    for r in draws:
        f = cap.cbf(r, 11, blobs)
        if f is not None and r['vcb'][11][3] >= 1024:
            m = cap.rigid_bones(f)
            if len(m) >= 72:
                gloves = m
                break
    R, T = gloves[0][:, :3], gloves[0][:, 3]
    ground = T[2]
    print('camera', v[7, :3], 'feet', T, 'light', L)

    # 1. the arcs: dark pixels in the ground region right of the gauntlet (x>300) and above the blob (y<260)
    pix, im = dark_pixels(img)
    arcs = [(x, y) for x, y in pix if 300 < x < 560 and 150 < y < 260]
    print(f'{len(arcs)} arc pixels')
    W = np.array([eye_to_ground(v, x + 0.5, y + 0.5, ground) for x, y in arcs])
    loc = (W - T) @ R  # root frame (x forward, y left, z up)
    big = loc[[y < 205 for x, y in arcs]]
    small = loc[[y >= 205 for x, y in arcs]]
    for nm, s in (('big arc', big), ('small arc', small)):
        if len(s):
            rr = np.hypot(s[:, 0], s[:, 1])
            print(f'{nm}: n={len(s)} root-frame x {s[:,0].min():.3f}..{s[:,0].max():.3f} y {s[:,1].min():.3f}..{s[:,1].max():.3f} '
                  f'dist from feet {rr.min():.3f}..{rr.max():.3f} (mean {rr.mean():.3f}) azimuth {np.degrees(np.arctan2(s[:,1],s[:,0])).min():.1f}..'
                  f'{np.degrees(np.arctan2(s[:,1],s[:,0])).max():.1f}')
    np.save('arcs_world.npy', W)
    # circle fit of the big arc (centre, radius) in root frame
    if len(big) > 10:
        A = np.c_[2 * big[:, 0], 2 * big[:, 1], np.ones(len(big))]
        bb = (big[:, :2] ** 2).sum(1)
        c = np.linalg.lstsq(A, bb, rcond=None)[0]
        rad = np.sqrt(c[2] + c[0] ** 2 + c[1] ** 2)
        print(f'big arc circle fit: centre ({c[0]:.3f},{c[1]:.3f}) radius {rad:.3f} (root frame)')
    centre = W.mean(axis=0)
    print('arcs centre world', centre)

    # 2. per-object positions within 1.5 m of the arcs, any pass
    print('\n(2) draws whose per-object position lies within 1.5 m of the arcs')
    seen = {}
    for r in draws:
        e = r['vcb'].get(11)
        f = cap.cbf(r, 11, blobs)
        if e is None or f is None:
            continue
        pos = None
        if e[3] >= 1024 and len(f) >= 44:
            m = cap.rigid_bones(f)
            if len(m):
                pos = m[0, :, 3]
        elif e[3] == 128:
            M4 = f[:16].reshape(4, 4)
            pos = M4[:3, 3] if np.abs(M4[3, :3]).max() < 1e-3 else M4[3, :3]
        if pos is None or np.linalg.norm(pos[:2] - centre[:2]) > 1.5 or abs(pos[2] - ground) > 3:
            continue
        key = (e[5], r['vb'].get(0, ('-',))[0])
        s = seen.setdefault(key, [e[3], pos, [], r.get('vsh')])
        s[2].append(f"{r['i']}:{body.pass_label(r, blobs)}:n{r['n']}x{r['inst']}")
    for (bid, vb), (w, pos, ds, vsh) in seen.items():
        print(f'  b{bid} cb{w} vb={vb[-8:]} vsh={vsh} pos-in-root={(pos - T) @ R} {" ".join(ds[:8])}')
    # (sun shadows: project.py, with joints; palette translations are NOT joint positions)

if __name__ == '__main__':
    main()
