"""Finds every bone palette bound to the VS in a capture, in any cb slot, any 16-byte start.

For each distinct (slot, blob) it reports the longest run of rigid 3x4 row-major matrices,
where it starts (float index), its translations' range, the number of draws reading it and
the passes they belong to.

    python palettes.py cap_sw [--min 4]
"""
import sys
from collections import Counter, defaultdict

import numpy as np

import cap


def best_run(f, tol=0.02):
    """Longest run of rigid r34 matrices over all 4-float aligned starts. -> (start, count)."""
    best = (0, 0)
    n4 = len(f) // 4
    if n4 < 3:
        return best
    rows = f[:n4 * 4].reshape(n4, 4)
    # candidate matrix at row k: rows k,k+1,k+2
    if n4 < 3:
        return best
    R = np.stack([rows[:-2, :3], rows[1:-1, :3], rows[2:, :3]], axis=1)
    with np.errstate(all='ignore'):
        e = np.abs(np.einsum('nij,nkj->nik', R, R) - np.eye(3)).max(axis=(1, 2))
    good = np.isfinite(e) & (e < tol) & (np.abs(np.linalg.det(np.nan_to_num(R)) - 1) < 0.05)
    for s in range(3):
        k = s
        while k < len(good):
            if good[k]:
                j = k
                while j < len(good) and good[j]:
                    j += 3
                cnt = (j - k) // 3
                if cnt > best[1]:
                    best = (k * 4, cnt)
                k = j
            k += 3
    return best


def main():
    name = sys.argv[1]
    mn = int(sys.argv[sys.argv.index('--min') + 1]) if '--min' in sys.argv else 4
    draws, shaders, layouts, blobs = cap.load(name)
    seen = {}
    users = defaultdict(list)
    for r in draws:
        for slot, e in r['vcb'].items():
            key = (slot, e[5])
            users[key].append(r)
            if key in seen:
                continue
            b = blobs.get(e[5])
            if b is None or len(b) < 48:
                seen[key] = (0, 0)
                continue
            f = np.frombuffer(b[:len(b) // 4 * 4], '<f4')
            seen[key] = best_run(f)
    by_slot = defaultdict(list)
    for key, (start, cnt) in seen.items():
        if cnt >= mn:
            by_slot[key[0]].append((key, start, cnt))
    for slot in sorted(by_slot):
        lst = by_slot[slot]
        print(f'-- slot cb{slot}: {len(lst)} blobs with >= {mn} rigid matrices; '
              f'start floats {Counter(s for _, s, _ in lst).most_common(5)}; bones {Counter(c for *_, c in lst).most_common(12)}')
        for (sl, bid), start, cnt in sorted(lst, key=lambda x: users[x[0]][0]['i'])[:400]:
            b = np.frombuffer(blobs[bid][:len(blobs[bid]) // 4 * 4], '<f4')
            m = b[start:start + cnt * 12].reshape(cnt, 3, 4)
            t = m[:, :, 3]
            us = users[(sl, bid)]
            bw = us[0]['vcb'][sl][3]
            print(f'   b{bid:<5} bw={bw:<6} start={start:<3} bones={cnt:<3} draws={len(us):<3} i={[u["i"] for u in us][:6]} '
                  f'vsh={sorted(set(u.get("vsh") for u in us))[:3]} passes={dict(Counter(cap.rt_tag(u) for u in us))} '
                  f't0=({t[0,0]:.2f},{t[0,1]:.2f},{t[0,2]:.2f}) tmean=({t[:,0].mean():.2f},{t[:,1].mean():.2f},{t[:,2].mean():.2f})')


if __name__ == '__main__':
    main()
