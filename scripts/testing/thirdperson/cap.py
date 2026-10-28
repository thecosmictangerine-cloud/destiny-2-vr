"""Shared loader for SVR_GpuCapture text+bin pairs (format of vr_gpu.cpp record_draw).

Each draw record is parsed into a dict with structured slots:
  vcb[slot] = (ptr, first, num, bytewidth, usage, blob)
  pcb[slot] = same for the pixel stage
  vt[slot]  = (ptr, dim, fmt, extra...) VS SRVs
  vb[slot]  = (ptr, stride, offset, bytewidth, usage)
"""
import os
import struct
import sys

import numpy as np

# The captures (cap_1p, cap_sw, cap_sp: SVR_GpuCapture.txt/.bin renamed) live outside the repo: set SVR_CAPTURES.
R1 = os.environ.get('SVR_CAPTURES', os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'r1'))


def load(name, base=R1):
    txt = os.path.join(base, name + '.txt')
    binp = os.path.join(base, name + '.bin')
    raw = open(binp, 'rb').read()
    blobs = {}
    i = 0
    while i + 8 <= len(raw):
        bid, size = struct.unpack_from('<II', raw, i)
        blobs[bid] = raw[i + 8:i + 8 + size]
        i += 8 + size
    draws, shaders, layouts = [], {}, {}
    for line in open(txt, encoding='utf-8', errors='replace').read().splitlines():
        if line.startswith('d '):
            rec = {'vcb': {}, 'pcb': {}, 'vt': {}, 'pt': {}, 'vb': {}}
            for tok in line[2:].split():
                if '=' not in tok:
                    rec[tok] = True
                    continue
                k, v = tok.split('=', 1)
                p = v.split(':')
                if k.startswith('cb') and k[2:].isdigit():
                    rec['vcb'][int(k[2:])] = (p[0], int(p[1]), int(p[2]), int(p[3]), p[4], int(p[5][1:]))
                elif k.startswith('pcb') and k[3:].isdigit():
                    rec['pcb'][int(k[3:])] = (p[0], int(p[1]), int(p[2]), int(p[3]), p[4], int(p[5][1:]))
                elif k.startswith('vb') and k[2:].isdigit():
                    rec['vb'][int(k[2:])] = (p[0], int(p[1]), int(p[2]), int(p[3]), p[4])
                elif k.startswith('pt') and k[2:].isdigit():
                    rec['pt'][int(k[2:])] = p
                elif k.startswith('t') and k[1:].isdigit():
                    rec['vt'][int(k[1:])] = p
                else:
                    rec[k] = v
            rec['i'] = int(rec['i'])
            rec['n'] = int(rec.get('n', 0))
            rec['inst'] = int(rec.get('inst', 1))
            draws.append(rec)
        elif line.startswith('s '):
            parts = line.split(' ', 3)
            shaders[parts[1]] = (parts[2][4:], parts[3] if len(parts) > 3 else '')
        elif line.startswith('l '):
            parts = line.split(' ', 2)
            layouts[parts[1]] = parts[2] if len(parts) > 2 else ''
    return draws, shaders, layouts, blobs


def cbf(rec, slot, blobs, stage='vcb'):
    e = rec[stage].get(slot)
    if e is None:
        return None
    b = blobs.get(e[5])
    if b is None or len(b) < 16:
        return None
    return np.frombuffer(b[:len(b) // 4 * 4], '<f4')


def view(rec, blobs):
    f = cbf(rec, 12, blobs)
    if f is None or len(f) < 56:
        return None
    return f[:56].reshape(14, 4)


def rigid_bones(f, start=32, tol=0.05):
    rest = f[start:]
    n = len(rest) // 12
    m = rest[:n * 12].reshape(n, 3, 4)
    R = m[:, :, :3]
    with np.errstate(all='ignore'):
        e = np.abs(np.einsum('nij,nkj->nik', R, R) - np.eye(3)).max(axis=(1, 2))
    k = 0
    while k < n and np.isfinite(e[k]) and e[k] < tol:
        k += 1
    return m[:k]


def rt_tag(rec):
    rt = rec.get('rt', '-')
    ds = rec.get('ds', '-')
    return f"{rt.split(':')[-1] if rt != '-' else 'nort'}{'+rt1' if 'rt1' in rec else ''}{'+ds' if ds != '-' else ''}"


if __name__ == '__main__':
    d, s, l, b = load(sys.argv[1])
    print(len(d), 'draws', len(s), 'shaders', len(b), 'blobs')
