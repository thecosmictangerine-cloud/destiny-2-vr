"""The faulting thread of a crash dump: exception, registers (named by module + offset), and every
stack qword that points into a module.

    pip install minidump
    python crash_dump.py %LOCALAPPDATA%\\CrashDumps\\destiny2.exe.<pid>.dmp [stack bytes, hex]

Windows writes the dump there (and the event log names `destiny2.exe+offset`). The stack scan is NOT
an unwind: values left over from earlier calls show up too (ours, in the CRT, deep in the stack, are
usually such leftovers), and the dump carries no memory map, so prehook thunks are invisible. Read the
registers first: in the dual-wield crashes of 2026-10-17 `rcx` held the weapon object.
"""
import ntpath
import struct
import sys

from minidump.minidumpfile import MinidumpFile


def main():
    mf = MinidumpFile.parse(sys.argv[1])
    span = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x2000
    mods = [(m.baseaddress, m.baseaddress + m.size, ntpath.basename(m.name)) for m in mf.modules.modules]

    def where(a):
        for b, e, n in mods:
            if b <= a < e:
                return f'{n}+0x{a - b:X}'
        return None

    exc = mf.exception.exception_records[0]
    er = exc.ExceptionRecord
    print('exception', er.ExceptionCode, 'at', where(er.ExceptionAddress),
          'params', [hex(x) for x in er.ExceptionInformation[:er.NumberParameters]])
    th = [t for t in mf.threads.threads if t.ThreadId == exc.ThreadId][0]
    print('thread', exc.ThreadId)
    ctx = th.ContextObject
    for r in ('Rip', 'Rsp', 'Rax', 'Rbx', 'Rcx', 'Rdx', 'Rsi', 'Rdi', 'R8', 'R9', 'R10', 'R11', 'R12', 'R13', 'R14', 'R15'):
        v = getattr(ctx, r, None)
        if v is not None:
            print(f'  {r}=0x{v:X} {where(v) or ""}')
    data = mf.get_reader().read(ctx.Rsp, span)
    print('stack (module addresses):')
    for i in range(0, len(data) - 8, 8):
        v = struct.unpack_from('<Q', data, i)[0]
        w = where(v)
        if w:
            print(f'  rsp+0x{i:X} {w}')


if __name__ == '__main__':
    main()
