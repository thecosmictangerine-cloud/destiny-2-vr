"""Offline prototype of the arm rig reconstruction, against a GPU capture and a dumped vertex buffer.

What it proves before any of it goes into C++:
  1. the vertex decode (R16G16B16A16_SNORM * scale + offset, weights, indices) -- by skinning the
     mesh with the captured palette, projecting it with the captured viewmodel camera and drawing
     it over the screenshot taken at the same moment;
  2. the joints, from where the vertex weights of two bones overlap (a joint is where the skin
     blends from one bone to the next, so the weight-product centroid sits on it), and the
     skeleton's topology from how much weight each pair shares.

Usage:
  python arm_rig.py --vb SVR_Vb_<ptr>.bin --draw <capture draw index> --shot shot.png --out overlay.png
"""
import argparse
import struct
from collections import defaultdict

import numpy as np

import gpu_capture as gc

GAME = r'C:\Games\Sunrise'


def decode_vb(raw, scale, offset, stride=16):
    n = len(raw) // stride
    a = np.frombuffer(raw[:n * stride], dtype=np.uint8).reshape(n, stride)
    pos = a[:, 0:8].copy().view('<i2').reshape(n, 4).astype(np.float32) / 32767.0
    w = a[:, 8:12].astype(np.float32) / 255.0
    idx = a[:, 12:16].astype(np.int32)
    p = pos[:, :3] * scale + offset
    return p, w, idx, pos[:, 3]


def palette(blob, bones=None):
    f = np.frombuffer(blob, '<f4')
    hdr = f[16:32].reshape(4, 4)
    rest = f[32:]
    n = len(rest) // 12 if bones is None else bones
    m = rest[:n * 12].reshape(n, 3, 4)
    return hdr, m


def rigid_count(m):
    R = m[:, :, :3]
    e = np.abs(np.einsum('nij,nkj->nik', R, R) - np.eye(3)).max(axis=(1, 2))
    k = 0
    while k < len(m) and e[k] < 0.05:
        k += 1
    return k


def skin(p, w, idx, m):
    out = np.zeros_like(p)
    for k in range(4):
        M = m[np.clip(idx[:, k], 0, len(m) - 1)]
        q = np.einsum('nij,nj->ni', M[:, :, :3], p) + M[:, :, 3]
        out += w[:, k:k + 1] * q
    return out


def project(points, cb12, size):
    """cb12 rows 0-3 hold the transposed view-projection; camera-relative points."""
    VP = cb12[0:4]  # row r = world component r, column c = clip component c
    h = np.c_[points, np.ones(len(points))] @ VP
    with np.errstate(all='ignore'):
        x = h[:, 0] / h[:, 3]
        y = h[:, 1] / h[:, 3]
    W, H = size
    return np.c_[(x * 0.5 + 0.5) * W, (0.5 - y * 0.5) * H], h[:, 3]


def joints(p, w, idx, nb):
    mass = np.zeros(nb)
    cen = np.zeros((nb, 3))
    pair_m = defaultdict(float)
    pair_p = defaultdict(lambda: np.zeros(3))
    for k in range(4):
        b = idx[:, k]
        ok = (w[:, k] > 0) & (b < nb)
        np.add.at(mass, b[ok], w[ok, k])
        np.add.at(cen, b[ok], w[ok, k:k + 1] * p[ok])
    for i in range(4):
        for j in range(i + 1, 4):
            wij = w[:, i] * w[:, j]
            ok = (wij > 1e-3) & (idx[:, i] != idx[:, j])
            for v in np.where(ok)[0]:
                a, b = sorted((idx[v, i], idx[v, j]))
                pair_m[(a, b)] += wij[v]
                pair_p[(a, b)] += wij[v] * p[v]
    with np.errstate(all='ignore'):
        cen = cen / mass[:, None]
    J = {k: pair_p[k] / pair_m[k] for k in pair_m}
    return mass, cen, dict(pair_m), J


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--vb', required=True)
    ap.add_argument('--draw', type=int, required=True)
    ap.add_argument('--shot')
    ap.add_argument('--out', default='rig_overlay.png')
    ap.add_argument('--game', default=GAME)
    a = ap.parse_args()
    draws, shaders, layouts, blobs = gc.load(a.game)
    rec = next(r for r in draws if r['kind'] == 'd' and int(r['i']) == a.draw)
    pal_blob = blobs[int(rec['cb11'].rsplit(':b', 1)[1])]
    cb12 = np.frombuffer(blobs[int(rec['cb12'].rsplit(':b', 1)[1])], '<f4').reshape(-1, 4)
    hdr, m_all = palette(pal_blob)
    nb = rigid_count(m_all)
    m = m_all[:nb]
    scale, offset = hdr[0, 0], hdr[1, :3]
    raw = open(a.vb, 'rb').read()
    p, w, idx, pw = decode_vb(raw, scale, offset)
    print(f'vertices {len(p)} bones {nb} scale {scale:.4f} offset {offset} '
          f'idx max {idx.max()} weight sums {np.percentile(w.sum(1), [1, 50, 99])}')
    print('bind bbox', p.min(0), p.max(0))
    sk = skin(p, w, idx, m)
    print('skinned bbox (camera-relative)', sk.min(0), sk.max(0))
    mass, cen, pm, J = joints(p, w, idx, nb)
    strong = sorted(pm.items(), key=lambda kv: -kv[1])
    print('strongest bone pairs:', [(k, round(v, 1)) for k, v in strong[:40]])
    np.save(a.out.replace('.png', '_rig.npy'), dict(mass=mass, cen=cen, pm=pm, J=J), allow_pickle=True)
    if a.shot:
        from PIL import Image, ImageDraw
        im = Image.open(a.shot).convert('RGB')
        d = ImageDraw.Draw(im)
        xy, wclip = project(sk[::7], cb12, im.size)
        for (x, y), ww in zip(xy, wclip):
            if np.isfinite(x) and ww != 0:
                d.point((x, y), fill=(255, 0, 255))
        # joints, posed with the first bone of the pair
        for (i, j), pos in J.items():
            if pm[(i, j)] < 3:
                continue
            q = m[i, :, :3] @ pos + m[i, :, 3]
            (x, y), = project(q[None], cb12, im.size)[0]
            if np.isfinite(x):
                d.ellipse((x - 4, y - 4, x + 4, y + 4), outline=(255, 255, 0))
                d.text((x + 5, y - 5), f'{i}-{j}', fill=(255, 255, 0))
        im.save(a.out)
        print('overlay ->', a.out)


if __name__ == '__main__':
    main()
