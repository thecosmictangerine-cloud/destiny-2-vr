"""Project the body's joints, the hands, the sword and their SHADOWS (along the sun, onto the ground plane at the
feet) into the composed eye image, to tell what the leftover shapes in it are.

The eye image (SVR_seen_*_eye0.bmp, 616x680) is the mock's composition: eye FOV -54/40 x +44/-55 deg, same
orientation as the render view (cyclopean, IPD offset ignored). World -> render clip via cb12 rows 0-3.

    python project.py cap_cloth <eye0.bmp> <out.png>
"""
import sys

import numpy as np
from PIL import Image, ImageDraw

import body
import cap

# bind-space joints (keypoints.py, docs/ARMS.md)
JOINTS = {
    'hipL': (3, (0.031, 0.105, 1.006)), 'hipR': (4, (0.040, -0.103, 1.014)), 'spine': (8, (0.015, 0.085, 1.154)),
    'neck': (16, (-0.021, 0.005, 1.61)), 'head': (18, (-0.004, 0.005, 1.671)),
    'shL': (15, (-0.027, 0.186, 1.485)), 'shR': (17, (-0.027, -0.186, 1.485)),
    'wrL': (21, (0.292, 0.392, 1.156)), 'wrR': (22, (0.292, -0.392, 1.156)),
    'ankL': (9, (-0.043, 0.172, 0.122)), 'ankR': (10, (-0.041, -0.173, 0.126)),
}
EYE = dict(l=-54, r=40, u=44, d=-55, w=616, h=680)


def main_view(draws, blobs):
    for r in draws:
        v = cap.view(r, blobs)
        if v is not None and body.view_kind(v) == 'main' and cap.rt_tag(r).startswith('f29+rt1'):
            return v


def csm_light_dir(draws, blobs):
    for r in draws:
        v = cap.view(r, blobs)
        if v is not None and body.view_kind(v) == 'ortho' and r.get('vp', '').startswith('0,0,1536'):
            M = v[0:4].T
            d = np.cross(M[0, :3], M[1, :3])
            d /= np.linalg.norm(d)
            return -d if d[2] > 0 else d  # direction the light travels (downward)


def to_eye(v, p):
    M = v[0:4].T
    c = M @ np.append(p, 1.0)
    if c[3] <= 1e-6:
        return None
    tx = (c[0] / c[3]) / v[10, 0]
    ty = (c[1] / c[3]) / v[11, 1]
    l, r, u, d = (np.tan(np.radians(EYE[k])) for k in 'lrud')
    return ((tx - l) / (r - l) * EYE['w'], (u - ty) / (u - d) * EYE['h'])


def main():
    name, img, out = sys.argv[1], sys.argv[2], sys.argv[3]
    draws, shaders, layouts, blobs = cap.load(name)
    v = main_view(draws, blobs)
    L = csm_light_dir(draws, blobs)
    print('camera', v[7, :3], 'view rows 4-6', v[4:7, :3].round(3).tolist())
    print('light travels along', L.round(3), f'elevation {np.degrees(np.arcsin(-L[2])):.1f} deg')
    pal = sword = blade = None
    for r in draws:
        f = cap.cbf(r, 11, blobs)
        if f is None or r['vcb'][11][3] < 1024:
            continue
        m = cap.rigid_bones(f)
        if len(m) >= 72 and pal is None:
            pal = m
        if len(m) == 1 and abs(f[16] - 0.4106) < 1e-3:
            blade = m[0]
    feet = pal[0, :, 3]
    ground = feet[2]
    im = Image.open(img).convert('RGB').resize((EYE['w'] * 2, EYE['h'] * 2))
    dr = ImageDraw.Draw(im)
    pts = {}
    for k, (b, J) in JOINTS.items():
        pts[k] = pal[b, :, :3] @ np.array(J) + pal[b, :, 3]
    if blade is not None:
        for z in (0.0, 0.26, 0.67, 1.08):
            pts[f'blade{z}'] = blade[:, :3] @ np.array([0, 0, z]) + blade[:, 3]

    def shadow(p):
        t = (ground - p[2]) / L[2]
        return p + t * L

    for k, p in pts.items():
        e = to_eye(v, p)
        s = to_eye(v, shadow(p))
        print(f'{k:10s} world=({p[0]:.3f},{p[1]:.3f},{p[2]:.3f}) h={p[2]-ground:.2f} eye={None if e is None else tuple(round(x) for x in e)} '
              f'shadow_eye={None if s is None else tuple(round(x) for x in s)}')
        for q, col in ((e, (255, 0, 0)), (s, (0, 255, 255))):
            if q is not None and -50 < q[0] < 700 and -50 < q[1] < 750:
                x, y = q[0] * 2, q[1] * 2
                dr.ellipse((x - 5, y - 5, x + 5, y + 5), outline=col, width=2)
                dr.text((x + 7, y - 7), k, fill=col)
    # vertical lines (plumb) from a few body points down to the ground: how a hanging rod would look
    for k in ('hipL', 'hipR', 'spine'):
        a, b2 = to_eye(v, pts[k]), to_eye(v, np.array([pts[k][0], pts[k][1], ground]))
        if a and b2:
            dr.line((a[0] * 2, a[1] * 2, b2[0] * 2, b2[1] * 2), fill=(255, 255, 0), width=1)
    im.save(out)
    print('wrote', out)


if __name__ == '__main__':
    main()
