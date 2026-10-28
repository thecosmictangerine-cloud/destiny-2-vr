"""Q1 survey: views (cb12), palettes (cb11 sizes), VS SRV buffers, passes, per capture.

    python survey.py cap_sw
"""
import sys
from collections import Counter, defaultdict

import numpy as np

import cap


def main():
    name = sys.argv[1]
    draws, shaders, layouts, blobs = cap.load(name)
    print(f'== {name}: {len(draws)} draws')
    # cb11 sizes (VS stage only)
    sizes = Counter(r['vcb'][11][3] for r in draws if 11 in r['vcb'])
    print('VS cb11 bytewidth -> draws:', dict(sizes))
    # views
    views = defaultdict(list)
    for r in draws:
        v = cap.view(r, blobs)
        if v is None:
            continue
        views[r['vcb'][12][5]].append(r['i'])
    print(f'distinct cb12 blobs: {len(views)}')
    for bid, idx in sorted(views.items(), key=lambda kv: kv[1][0]):
        r = next(x for x in draws if x['i'] == idx[0])
        v = cap.view(r, blobs)
        cam = v[7, :3]
        P = v[10:14]
        tags = Counter(cap.rt_tag(next(x for x in draws if x['i'] == i)) for i in idx[:4000])
        print(f'  b{bid:<5} draws={len(idx):4d} first={idx[0]:4d} last={idx[-1]:4d} cam=({cam[0]:9.3f},{cam[1]:9.3f},{cam[2]:8.3f}) '
              f'P00={P[0,0]:.3f} P11={P[1,1]:.3f} P22={P[2,2]:.4f} P23={P[2,3]:.4f} P32={P[3,2]:.4f} P33={P[3,3]:.3f} '
              f'rows4={np.round(v[4,:3],3)} passes={dict(tags.most_common(4))}')
    # VS SRVs that are buffers
    srv = Counter()
    for r in draws:
        for s, p in r['vt'].items():
            if len(p) > 2 and p[1].startswith('BUF'):
                srv[(s, p[1], p[-1].split('b')[0] if False else p[4] if len(p) > 4 else '')] += 1
    print('VS SRV buffer slots:', srv.most_common(12))
    print('VS SRV dims:', Counter((s, p[1]) for r in draws for s, p in r['vt'].items()).most_common(12))


if __name__ == '__main__':
    main()
