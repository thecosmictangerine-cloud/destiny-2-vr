"""Every float3 (at a 16-byte row start) in ANY constant buffer of any draw/stage within --radius of the feet, excluding
cb11 palettes: per-frame positions the engine hands its shaders (camera, light, player position, effect origins).
Grouped by value; lists the slots, row offsets and passes it appears in.

    python nearvec.py cap_arcs [--radius 6]
"""
import sys
from collections import OrderedDict

import numpy as np

import anyrigid
import body
import cap


def main():
    name = sys.argv[1]
    radius = float(sys.argv[sys.argv.index('--radius') + 1]) if '--radius' in sys.argv else 6.0
    draws, shaders, layouts, blobs = cap.load(name)
    root = anyrigid.feet_of(draws, blobs)
    R, T = root[:, :3], root[:, 3]
    g = OrderedDict()
    seenblob = {}
    for r in draws:
        for stage in ('vcb', 'pcb'):
            for slot, e in r[stage].items():
                if slot == 11 and e[3] >= 1024:
                    continue
                bid = e[5]
                if bid not in seenblob:
                    b = blobs.get(bid, b'')
                    f = np.frombuffer(b[:len(b) // 16 * 16], '<f4').reshape(-1, 4) if len(b) >= 16 else np.zeros((0, 4))
                    hits = []
                    with np.errstate(all='ignore'):
                        d = np.linalg.norm(f[:, :3] - T, axis=1)
                    for row in np.nonzero(d < radius)[0]:
                        hits.append((row, tuple(np.round(f[row, :3], 3)), float(f[row, 3])))
                    seenblob[bid] = hits
                for row, v, w in seenblob[bid]:
                    key = v
                    s = g.setdefault(key, dict(where=OrderedDict(), w=w))
                    tag = f'{stage}{slot}r{row}'
                    s['where'].setdefault(tag, []).append(f"{r['i']}:{body.pass_label(r, blobs)}")
    for v, s in g.items():
        p = np.array(v)
        loc = (p - T) @ R
        print(f"({v[0]:.3f},{v[1]:.3f},{v[2]:.3f}) w={s['w']:.3f} root=({loc[0]:.2f},{loc[1]:.2f},{loc[2]:.2f}) "
              + ' | '.join(f"{t} x{len(d)} [{d[0]}..{d[-1]}]" for t, d in list(s['where'].items())[:6]))


if __name__ == '__main__':
    main()
