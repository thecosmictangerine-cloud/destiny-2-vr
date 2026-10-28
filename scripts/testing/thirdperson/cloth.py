"""The CPU-simulated cloth of the Guardian's class item, in every capture given.

Identified by its vertex shader's inputs (POSITION/NORMAL/TANGENT as full float4, no BLENDINDICES read) and
its input layout (slot 0: R32G32B32A32_FLOAT x3, stride 48, a DYNAMIC buffer). Prints, per draw: pass, VB
(ptr, offset, bytewidth, usage), index range, the 128-byte cb11 (rows 0-3 world matrix, rows 4-5 the mesh
header: dequantisation scale and origin) and, for comparison, every armour palette in the same capture with
the same header -- the skinned part of the same item, which our collapse zeroes.

    python cloth.py cap_cloth cap_sw cap_1p cap_sp ...
"""
import sys

import numpy as np

import body
import cap


def cloth_layouts(layouts):
    out = set()
    for ptr, desc in layouts.items():
        toks = desc.split()
        if 'POSITION0:f2:s0:o4294967295' in toks and 'NORMAL0:f2:s0:o4294967295' in toks:
            out.add(ptr)
    return out


def main():
    np.set_printoptions(precision=4, suppress=True, linewidth=150)
    for name in sys.argv[1:]:
        draws, shaders, layouts, blobs = cap.load(name)
        cam = body.main_camera(draws, blobs)
        ils = cloth_layouts(layouts)
        print(f'===== {name}: main camera {cam}  cloth layouts {[p[-6:] for p in ils]}')
        headers = {}
        feet = None
        for r in draws:
            e = r['vcb'].get(11)
            f = cap.cbf(r, 11, blobs)
            if e is None or f is None or e[3] < 1024 or len(f) < 44:
                continue
            m = cap.rigid_bones(f)
            if len(m) >= 52 and feet is None:
                feet = m[0, :, 3]
            key = (round(float(f[16]), 4), tuple(np.round(f[20:23], 4)))
            state = 'zero' if np.abs(f[32:44]).max() == 0 else f'{len(m)} bones root {np.round(m[0, :, 3], 3) if len(m) else "-"}'
            headers.setdefault(key, set()).add((e[3], state))
        print('   player root (feet)', feet)
        for r in draws:
            if r.get('il') not in ils:
                continue
            f = cap.cbf(r, 11, blobs)
            vb0 = r['vb'][0]
            key = (round(float(f[16]), 4), tuple(np.round(f[20:23], 4)))
            ident = np.allclose(f[:16].reshape(4, 4), np.eye(4), atol=1e-6)
            print(f"   i={r['i']} {body.pass_label(r, blobs):12s} vsh={r['vsh']} psh={r.get('psh')} vb0={vb0[0][-8:]} stride={vb0[1]} off={vb0[2]} "
                  f"bw={vb0[3]} {vb0[4]} ib={r['ib'].split(':')[0][-8:]} si={r.get('si')} n={r['n']} "
                  f"cb11 bw={r['vcb'][11][3]} world=identity:{ident} header scale={key[0]} origin={key[1]}")
            print(f"      same header among armour palettes: {headers.get(key, 'NONE')}")
            vs = r['vb']
            print(f"      other streams: {[(s, v[0][-8:], v[1], v[3], v[4]) for s, v in vs.items() if s != 0]}")


if __name__ == '__main__':
    main()
