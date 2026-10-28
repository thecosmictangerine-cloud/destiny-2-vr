"""Every draw that reads a DYNAMIC vertex buffer (usage u2), with its pass, layout, shaders, offsets and the
per-object constants (cb0 window, cb11), so CPU-written geometry (cloth, ribbons, particles) can be told apart.

    python dynvb.py cap_cloth
"""
import sys
from collections import OrderedDict

import numpy as np

import body
import cap


def main():
    name = sys.argv[1]
    draws, shaders, layouts, blobs = cap.load(name)
    cam = body.main_camera(draws, blobs)
    print(f'{name}: main camera {cam}')
    g = OrderedDict()
    for r in draws:
        dyn = [(s, v) for s, v in r['vb'].items() if v[4] == 'u2']
        if not dyn:
            continue
        lay = layouts.get(r.get('il'), '-')
        key = (tuple(v[0] for _, v in dyn), r.get('il'), r.get('vsh'))
        e = g.setdefault(key, dict(draws=[], lay=lay, dyn=dyn, passes=[]))
        e['draws'].append((r['i'], r['n'], r['inst'], tuple(v[2] for _, v in dyn), body.pass_label(r, blobs),
                           r['vcb'].get(11, (0, 0, 0, 0))[3], r['vcb'].get(0, (0, 0, 0, 0))[1:4]))
    for (vbs, il, vsh), e in g.items():
        print(f'== vbs={[v[-8:] for v in vbs]} slots={[(s, v[1], v[3]) for s, v in e["dyn"]]} il={il[-6:] if il else il} vsh={vsh}')
        print(f'   layout: {e["lay"]}')
        for d in e['draws'][:12]:
            print(f'   i={d[0]} n={d[1]} inst={d[2]} off={d[3]} pass={d[4]} cb11w={d[5]} cb0(first,num,bw)={d[6]}')
        if len(e['draws']) > 12:
            print(f'   ... {len(e["draws"])} draws')


if __name__ == '__main__':
    main()
