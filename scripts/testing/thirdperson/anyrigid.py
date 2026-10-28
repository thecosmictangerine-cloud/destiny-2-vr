"""Every rigid 3x4 / 4x4 block in ANY constant buffer of any draw (VS and PS, every slot, every 16-byte offset) whose
translation lies within --radius of the player's feet, grouped by (stage, slot, blob): what places geometry at the
player outside the cb11 palettes our rules rewrite.

    python anyrigid.py cap_arcs [--radius 3]
"""
import sys
from collections import OrderedDict

import numpy as np

import body
import cap


def feet_of(draws, blobs):
    for r in draws:
        f = cap.cbf(r, 11, blobs)
        if f is not None and r['vcb'][11][3] >= 1024:
            m = cap.rigid_bones(f)
            if len(m) >= 52:
                return m[0]


def rigid_hits(f, T, radius):
    out = []
    n = len(f)
    for o in range(0, n - 11, 4):
        # 3x4 row-major (rotation in [:, :3], translation in [:, 3])
        m = f[o:o + 12].reshape(3, 4)
        R = m[:, :3]
        if not np.all(np.isfinite(m)):
            continue
        if np.abs(R @ R.T - np.eye(3)).max() < 0.02 and np.linalg.norm(m[:, 3] - T) < radius:
            out.append(('3x4', o // 4, m[:, 3].copy()))
        if o + 16 <= n:
            M = f[o:o + 16].reshape(4, 4)
            R2 = M[:3, :3]
            if np.abs(R2 @ R2.T - np.eye(3)).max() < 0.02 and np.abs(M[:3, 3]).max() < 1e-4 and abs(M[3, 3] - 1) < 1e-4 \
                    and np.linalg.norm(M[3, :3] - T) < radius:
                out.append(('4x4T', o // 4, M[3, :3].copy()))
            if np.abs(R2 @ R2.T - np.eye(3)).max() < 0.02 and np.abs(M[3, :3]).max() < 1e-4 and abs(M[3, 3] - 1) < 1e-4 \
                    and np.linalg.norm(M[:3, 3] - T) < radius:
                out.append(('4x4', o // 4, M[:3, 3].copy()))
    return out


def main():
    name = sys.argv[1]
    radius = float(sys.argv[sys.argv.index('--radius') + 1]) if '--radius' in sys.argv else 3.0
    draws, shaders, layouts, blobs = cap.load(name)
    root = feet_of(draws, blobs)
    R, T = root[:, :3], root[:, 3]
    g = OrderedDict()
    cache = {}
    for r in draws:
        for stage in ('vcb', 'pcb'):
            for slot, e in r[stage].items():
                if slot == 12:
                    continue
                bid = e[5]
                if bid not in cache:
                    b = blobs.get(bid)
                    f = np.frombuffer(b[:len(b) // 4 * 4], '<f4') if b else np.zeros(0, '<f4')
                    with np.errstate(all='ignore'):
                        cache[bid] = rigid_hits(f, T, radius) if len(f) >= 12 else []
                hits = cache[bid]
                if not hits:
                    continue
                key = (stage, slot, bid)
                s = g.setdefault(key, dict(w=e[3], hits=hits, draws=[]))
                s['draws'].append(f"{r['i']}:{body.pass_label(r, blobs)}:n{r['n']}x{r['inst']}:{r.get('vsh')}")
    for (stage, slot, bid), s in g.items():
        h = s['hits']
        locs = ' '.join(f"{k}@{o}:({','.join(f'{x:.2f}' for x in (p - T) @ R)})" for k, o, p in h[:4])
        print(f"{stage}{slot} b{bid} w={s['w']} hits={len(h)} {locs}{' ...' if len(h) > 4 else ''}")
        print('      ' + ' '.join(s['draws'][:10]) + (f' ... ({len(s["draws"])})' if len(s['draws']) > 10 else ''))


if __name__ == '__main__':
    main()
