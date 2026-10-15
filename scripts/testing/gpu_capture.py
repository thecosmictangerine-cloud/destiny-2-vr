"""Reads SVR_GpuCapture.txt/.bin (written by hooks/vr/vr_gpu.cpp) and looks for bone palettes.

A bone palette is a run of rigid transforms. Whatever the layout, the rotation part of each one is
orthonormal, which random floats, colours or packed vertex data never are -- so the test is cheap
and almost free of false positives. Tried layouts, per matrix:

  r34   3 float4 rows, row-major affine (m00 m01 m02 tx | ...)        48 bytes  (Dishonored)
  c34   3 float4 rows holding the TRANSPOSED 3x3 plus a translation    48 bytes
  r44   4x4 row-major, last row (0 0 0 1)                              64 bytes
  c44   4x4 column-major, last column (0 0 0 1)                        64 bytes
  r43   4 float3 rows, rotation then translation                       48 bytes

Usage:  python gpu_capture.py [game_dir] [--draws] [--vs CHK] [--min N]
"""
import struct
import sys
from collections import Counter, defaultdict

import numpy as np

GAME = r'C:\Games\Sunrise'


def load(game):
    text = open(game + r'\SVR_GpuCapture.txt', encoding='utf-8', errors='replace').read().splitlines()
    raw = open(game + r'\SVR_GpuCapture.bin', 'rb').read()
    blobs = {}
    i = 0
    while i + 8 <= len(raw):
        bid, size = struct.unpack_from('<II', raw, i)
        blobs[bid] = raw[i + 8:i + 8 + size]
        i += 8 + size
    draws, shaders, layouts = [], {}, {}
    for line in text:
        if line.startswith('d ') or line.startswith('x '):
            rec = {'kind': line[0]}
            for tok in line[2:].split():
                if '=' in tok:
                    k, v = tok.split('=', 1)
                    rec[k] = v
                else:
                    rec[tok] = True
            draws.append(rec)
        elif line.startswith('s '):
            parts = line.split(' ', 3)
            shaders[parts[1]] = parts[3] if len(parts) > 3 else ''
            shaders[parts[1] + ':chk'] = parts[2][4:]
        elif line.startswith('l '):
            parts = line.split(' ', 2)
            layouts[parts[1]] = parts[2] if len(parts) > 2 else ''
    return draws, shaders, layouts, blobs


def rigid_run(f, layout):
    """Number of consecutive rigid matrices at the start of float array f for a layout."""
    if layout in ('r34', 'c34', 'r43'):
        n = len(f) // 12
        m = f[:n * 12].reshape(n, 12)
        if layout == 'r34':
            R = m[:, [0, 1, 2, 4, 5, 6, 8, 9, 10]].reshape(n, 3, 3)
        elif layout == 'c34':
            R = m[:, [0, 1, 2, 4, 5, 6, 8, 9, 10]].reshape(n, 3, 3).transpose(0, 2, 1)
        else:
            R = m[:, :9].reshape(n, 3, 3)
        ok_extra = np.ones(n, bool)
    else:
        n = len(f) // 16
        m = f[:n * 16].reshape(n, 4, 4)
        if layout == 'r44':
            R = m[:, :3, :3]
            ok_extra = np.all(np.abs(m[:, 3, :] - [0, 0, 0, 1]) < 1e-3, axis=1)
        else:
            R = m[:, :3, :3]
            ok_extra = np.all(np.abs(m[:, :, 3] - [0, 0, 0, 1]) < 1e-3, axis=1)
    if n == 0:
        return 0, n, None
    with np.errstate(all='ignore'):
        RtR = np.einsum('nij,nkj->nik', R, R)
        err = np.abs(RtR - np.eye(3)).max(axis=(1, 2))
    good = (err < 0.02) & ok_extra & np.all(np.isfinite(R), axis=(1, 2))
    run = 0
    while run < n and good[run]:
        run += 1
    return run, n, good


def palette_scan(blobs, min_run):
    hits = {}
    for bid, b in blobs.items():
        if len(b) < 48 * 4 or len(b) % 4:
            continue
        f = np.frombuffer(b, dtype='<f4')
        best = None
        for layout in ('r34', 'c34', 'r44', 'c44', 'r43'):
            run, n, good = rigid_run(f, layout)
            total = int(good.sum()) if good is not None else 0
            if total >= min_run and (best is None or total > best[2]):
                best = (layout, run, total, n)
        if best:
            hits[bid] = best
    return hits


def translations(b, layout, count=200):
    f = np.frombuffer(b, dtype='<f4')
    if layout in ('r34', 'c34'):
        n = min(len(f) // 12, count)
        m = f[:n * 12].reshape(n, 12)
        return m[:, [3, 7, 11]]
    if layout == 'r43':
        n = min(len(f) // 12, count)
        return f[:n * 12].reshape(n, 12)[:, 9:12]
    n = min(len(f) // 16, count)
    m = f[:n * 16].reshape(n, 4, 4)
    return m[:, :3, 3] if layout == 'r44' else m[:, 3, :3]


def main():
    args = sys.argv[1:]
    game = GAME
    if args and not args[0].startswith('--'):
        game = args.pop(0)
    min_run = int(args[args.index('--min') + 1]) if '--min' in args else 8
    draws, shaders, layouts, blobs = load(game)
    d = [r for r in draws if r['kind'] == 'd']
    x = [r for r in draws if r['kind'] == 'x']
    print(f'draws {len(d)}  dispatches {len(x)}  blobs {len(blobs)}  shaders {len(shaders)//2}')
    print('context types:', Counter('def' if 'def' in r else 'imm' for r in d))
    named = sum(1 for k, v in shaders.items() if not k.endswith(':chk') and "'" in v)
    print('shaders with names in reflection:', named)

    hits = palette_scan(blobs, min_run)
    print(f'\n== candidate palettes (>= {min_run} rigid matrices): {len(hits)}')
    # Which draws/slots read each candidate blob.
    readers = defaultdict(list)
    for r in d:
        for k, v in r.items():
            if (k.startswith('t') and k[1:].isdigit()) or (k.startswith('cb') and k[2:].isdigit()):
                bid = v.rsplit(':b', 1)[-1] if ':b' in v else None
                if bid and bid.isdigit() and int(bid) in hits:
                    readers[int(bid)].append((r, k))
    for bid, (layout, run, total, n) in sorted(hits.items(), key=lambda kv: -kv[1][2]):
        t = translations(blobs[bid], layout)
        rd = readers.get(bid, [])
        slots = Counter(k for _, k in rd)
        vss = Counter(r.get('vsh', '?') for r, _ in rd)
        tr = f'tx[{t[:,0].min():.2f},{t[:,0].max():.2f}] ty[{t[:,1].min():.2f},{t[:,1].max():.2f}] tz[{t[:,2].min():.2f},{t[:,2].max():.2f}]'
        print(f'b{bid:<5} {layout} run={run} rigid={total}/{n} bytes={len(blobs[bid])} draws={len(rd)} slots={dict(slots)} vs={dict(vss)} {tr}')

    if '--draws' in args:
        print('\n== skinned-looking draws (read a candidate palette)')
        for bid, rd in readers.items():
            for r, k in rd:
                print(f"  i={r['i']} {k} b{bid} n={r.get('n')} vsh={r.get('vsh')} vb0={r.get('vb0','-')} rt={r.get('rt','-')} vp={r.get('vp','-')}")
    if '--vs' in args:
        chk = args[args.index('--vs') + 1].upper()
        for k, v in shaders.items():
            if k.endswith(':chk') and v.upper().startswith(chk):
                ptr = k[:-4]
                print('\n', ptr, shaders[ptr])
                for r in d:
                    if r.get('vs') == ptr:
                        print('   ', ' '.join(f'{a}={b}' for a, b in r.items() if a not in ('kind',)))
                        break


if __name__ == '__main__':
    main()
