"""cap_hammer: the 6-bone class-item palette (scale 0.3611): each bone's rotation and where it carries points of
the item's bind box (header origin +- scale), in the root frame -- to tell the belt from the flaps -- and the
same points projected into the 60-deg-down eye image (head rotated down about the camera's right axis).

    python mark.py cap_hammer <down_eye0.bmp> <out.png>
"""
import sys

import numpy as np
from PIL import Image, ImageDraw

import body
import cap
import project

np.set_printoptions(precision=3, suppress=True, linewidth=160)


def main():
    name, img, out = sys.argv[1], sys.argv[2], sys.argv[3]
    draws, shaders, layouts, blobs = cap.load(name)
    gloves = mark = None
    for r in draws:
        f = cap.cbf(r, 11, blobs)
        if f is None or r['vcb'][11][3] < 1024:
            continue
        m = cap.rigid_bones(f)
        if len(m) >= 60 and gloves is None:
            gloves = m
        if abs(f[16] - 0.3611) < 1e-3 and len(m) == 6:
            mark, origin, scale = m, f[20:23].copy(), float(f[16])
    R0, T0 = gloves[0][:, :3], gloves[0][:, 3]
    for i, b in enumerate(mark):
        Rr = R0.T @ b[:, :3]
        ang = np.degrees(np.arccos(np.clip((np.trace(Rr) - 1) / 2, -1, 1)))
        print(f'bone {i}: rotation vs root {ang:.1f} deg; bind points ->')
        for p in ((0, 0, 0.85), (0.2, 0, 0.85), (0, 0.2, 0.85), (0, -0.2, 0.85), (0.2, 0, 0.5), (0, 0.2, 0.5), (0, -0.2, 0.5)):
            w = b[:, :3] @ np.array(p) + b[:, 3]
            print(f'      {p} -> {R0.T @ (w - T0)}')
    # The down view: the capture's main view turned 60 deg down about its own right axis (cyclopean eye).
    v = project.main_view(draws, blobs).copy()
    M = v[0:4].T  # clip = M @ world
    cam = v[7, :3]
    # camera basis from the view-projection: rows of the view rotation are recovered from M with the projection
    P = np.diag([v[10, 0], v[11, 1], 1, 1])
    print('camera', cam)
    im = Image.open(img).convert('RGB').resize((project.EYE['w'] * 2, project.EYE['h'] * 2))
    dr = ImageDraw.Draw(im)
    # Build a view directly: forward = root x axis, up = z, pitched 60 deg down.
    fwd0, up0 = R0[:, 0], R0[:, 2]
    right = np.cross(fwd0, up0)
    a = np.radians(-60)
    fwd = np.cos(a) * fwd0 + np.sin(a) * up0
    up = np.cross(right, fwd)
    l, r_, u, d = (np.tan(np.radians(project.EYE[k])) for k in 'lrud')

    def eye(p):
        q = p - cam
        z = q @ fwd
        if z <= 1e-3:
            return None
        x, y = (q @ right) / z, (q @ up) / z
        return ((x - l) / (r_ - l) * project.EYE['w'] * 2, (u - y) / (u - d) * project.EYE['h'] * 2)

    rng = np.random.default_rng(0)
    cols = [(255, 0, 0), (0, 255, 0), (0, 128, 255), (255, 255, 0), (255, 0, 255), (0, 255, 255)]
    for i, b in enumerate(mark):
        for _ in range(150):
            p = origin + scale * (rng.random(3) * 2 - 1)
            w = b[:, :3] @ p + b[:, 3]
            e = eye(w)
            if e is not None and 0 <= e[0] < im.width and 0 <= e[1] < im.height:
                dr.ellipse((e[0] - 2, e[1] - 2, e[0] + 2, e[1] + 2), fill=cols[i])
        e = eye(b[:, :3] @ origin + b[:, 3])
        if e:
            dr.text(e, f'b{i}', fill=cols[i])
    # belt circle: radius 0.2 m at 0.95 m up around the root
    for rad, hz, col in ((0.2, 0.95, (255, 255, 255)), (0.25, 0.85, (255, 0, 255)), (0.15, 1.05, (0, 255, 255))):
        for t in np.linspace(0, 2 * np.pi, 90):
            e = eye(T0 + R0 @ np.array([rad * np.cos(t), rad * np.sin(t), hz]))
            if e:
                dr.ellipse((e[0] - 3, e[1] - 3, e[0] + 3, e[1] + 3), fill=col)
    im.save(out)
    print('wrote', out)


if __name__ == '__main__':
    main()
