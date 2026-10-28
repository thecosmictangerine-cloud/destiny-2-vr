"""Every thread of a (hang) dump: where it is, and the module addresses near the top of its stack.

    python hang_threads.py <dump> [stack bytes, hex, default 0x600] [module filter, default steam_api64]

A hang is a set of threads waiting on each other: read the RIPs first (who is in a wait, who spins),
then the stack scan of the ones whose stacks hold our DLL (steam_api64.dll). The scan is not an unwind:
leftover values show up too.
"""
import ntpath
import struct
import sys
from collections import Counter

from minidump.minidumpfile import MinidumpFile


def main():
    mf = MinidumpFile.parse(sys.argv[1])
    span = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x600
    want = sys.argv[3].lower() if len(sys.argv) > 3 else 'steam_api64'
    mods = [(m.baseaddress, m.baseaddress + m.size, ntpath.basename(m.name)) for m in mf.modules.modules]

    def where(a):
        for b, e, n in mods:
            if b <= a < e:
                return f'{n}+0x{a - b:X}'
        return None

    reader = mf.get_reader()
    rips = Counter()
    for th in mf.threads.threads:
        ctx = th.ContextObject
        rip = getattr(ctx, 'Rip', 0)
        rsp = getattr(ctx, 'Rsp', 0)
        w = where(rip) or hex(rip)
        rips[w.split('+')[0]] += 1
        hits = []
        try:
            data = reader.read(rsp, span)
        except Exception:
            data = b''
        for i in range(0, len(data) - 7, 8):
            v = struct.unpack_from('<Q', data, i)[0]
            n = where(v)
            if n and (want in n.lower() or n.lower().startswith('destiny2')):
                hits.append(n)
        ours = [h for h in hits if want in h.lower()]
        if ours or 'ntdll' not in w.lower():
            print(f'tid={th.ThreadId} rip={w}')
            for h in hits[:14]:
                print('     ', h)
    print('rip modules:', dict(rips))


if __name__ == '__main__':
    main()
