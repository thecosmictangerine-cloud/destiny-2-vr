"""Diffs the constant buffers of one kind of draw between two GPU captures (e.g. hip vs ADS).

    python cb_diff.py <a.txt> <a.bin> <b.txt> <b.bin> <vsh> <psh> [--stage pcb|cb] [--min 1e-4]

Finds the first draw with that vertex/pixel shader checksum pair in each capture, reads every
constant buffer it bound (pixel stage `pcb`, or vertex stage `cb`) out of the blob file, and prints
each float that differs: slot, float index, value in a, value in b. A switch like "reticle opacity"
shows up as one float going from 0 to 1.
"""
import re
import struct
import sys


def blobs(path):
    data = open(path, 'rb').read()
    out = {}
    i = 0
    while i + 8 <= len(data):
        bid, size = struct.unpack_from('<II', data, i)
        i += 8
        out[bid] = data[i:i + size]
        i += size
    return out


def draw(txt, vsh, psh):
    for line in open(txt, encoding='utf-8', errors='replace'):
        if line.startswith('d ') and f'vsh={vsh}' in line and f'psh={psh}' in line:
            return line
    return None


def main():
    a_txt, a_bin, b_txt, b_bin, vsh, psh = sys.argv[1:7]
    stage = sys.argv[sys.argv.index('--stage') + 1] if '--stage' in sys.argv else 'pcb'
    tiny = float(sys.argv[sys.argv.index('--min') + 1]) if '--min' in sys.argv else 1e-4
    la, lb = draw(a_txt, vsh, psh), draw(b_txt, vsh, psh)
    if not la or not lb:
        print('draw not found in', 'a' if not la else 'b')
        return
    ba, bb = blobs(a_bin), blobs(b_bin)
    pat = re.compile(r' ' + stage + r'(\d+)=\w+:\d+:\d+:(\d+):u\d+:b(\d+)')
    ca = {int(m.group(1)): int(m.group(3)) for m in pat.finditer(la)}
    cb = {int(m.group(1)): int(m.group(3)) for m in pat.finditer(lb)}
    print('slots a', sorted(ca), 'b', sorted(cb))
    for slot in sorted(set(ca) & set(cb)):
        x, y = ba.get(ca[slot], b''), bb.get(cb[slot], b'')
        n = min(len(x), len(y)) // 4
        fx = struct.unpack(f'<{n}f', x[:n * 4])
        fy = struct.unpack(f'<{n}f', y[:n * 4])
        diffs = [(i, fx[i], fy[i]) for i in range(n) if abs(fx[i] - fy[i]) > tiny]
        print(f'slot {slot}: {n} floats, {len(diffs)} differ')
        for i, u, v in diffs[:60]:
            print(f'   [{i:3d}] (row {i // 4}.{"xyzw"[i % 4]})  {u: .6g}  ->  {v: .6g}')


if __name__ == '__main__':
    main()
