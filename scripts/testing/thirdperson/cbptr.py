"""Q6: every absolute palette in a capture, keyed by the cb11 BUFFER pointer (per-object buffers),
with its bone count, scale, root position, distance to the main camera, vertex buffers and cb0 window.

    python cbptr.py cap_sw
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
    rows = OrderedDict()
    for r in draws:
        e = r['vcb'].get(11)
        if e is None:
            continue
        sig = shaders.get(r.get('vs'), ('', ''))[1]
        if 'POSITION0:F' not in sig:
            continue  # float3-position shaders do not read a bone palette (stale cb11 binding)
        f = cap.cbf(r, 11, blobs)
        m = cap.rigid_bones(f) if f is not None and len(f) > 44 else []
        if len(m) == 0:
            continue
        v = cap.view(r, blobs)
        if v is not None and np.abs(v[7, :3]).max() < 1e-3:
            kind = 'VM'
        else:
            kind = 'abs'
        key = (e[0], e[5])
        d = rows.setdefault(key, dict(bw=e[3], bones=len(m), s=float(f[16]), root=m[0, :, 3].copy(), kind=kind,
                                     vbs=set(), draws=[], passes=set()))
        d['vbs'].add(r['vb'].get(0, ('-',))[0][-8:])
        d['draws'].append(r['i'])
        d['passes'].add(body.pass_label(r, blobs))
    print(f'{name}: main camera {cam}')
    for (ptr, bid), d in rows.items():
        dist = np.linalg.norm(d['root'] - cam) if d['kind'] == 'abs' else float('nan')
        print(f"cb11={ptr[-10:]} bw={d['bw']:<5} b{bid:<5} {d['kind']} bones={d['bones']:<3} s={d['s']:.4f} "
              f"root=({d['root'][0]:.2f},{d['root'][1]:.2f},{d['root'][2]:.2f}) dist={dist:7.2f} draws={len(d['draws'])} "
              f"first={d['draws'][0]} vbs={sorted(d['vbs'])} passes={sorted(d['passes'])}")


if __name__ == '__main__':
    main()
