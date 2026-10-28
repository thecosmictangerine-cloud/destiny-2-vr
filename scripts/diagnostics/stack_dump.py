"""A small dump of a running (or hung) game: every thread, its context and its stack, nothing else.

    python stack_dump.py <out.dmp> [pid]       (default: the running destiny2.exe)

MiniDumpNormal through dbghelp, written by this user (comsvcs' MiniDump writes an admin-only file).
Read it with hang_threads.py.
"""
import ctypes
import ctypes.wintypes as wt
import subprocess
import sys

PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ = 0x0010
MiniDumpNormal = 0x0
MiniDumpWithThreadInfo = 0x1000


def game_pid():
    out = subprocess.run(['tasklist', '/FI', 'IMAGENAME eq destiny2.exe', '/FO', 'CSV', '/NH'], capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = [p.strip('"') for p in line.split('","')]
        if len(parts) > 1 and parts[0].lower().startswith('destiny2'):
            return int(parts[1])
    return None


def main():
    out = sys.argv[1]
    pid = int(sys.argv[2]) if len(sys.argv) > 2 else game_pid()
    if pid is None:
        sys.exit('no destiny2.exe')
    k32 = ctypes.WinDLL('kernel32', use_last_error=True)
    dbghelp = ctypes.WinDLL('dbghelp', use_last_error=True)
    k32.OpenProcess.restype = wt.HANDLE
    h = k32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
    if not h:
        sys.exit(f'OpenProcess failed {ctypes.get_last_error()}')
    GENERIC_WRITE = 0x40000000
    CREATE_ALWAYS = 2
    k32.CreateFileW.restype = wt.HANDLE
    f = k32.CreateFileW(out, GENERIC_WRITE, 0, None, CREATE_ALWAYS, 0x80, None)
    ok = dbghelp.MiniDumpWriteDump(h, pid, f, MiniDumpNormal | MiniDumpWithThreadInfo, None, None, None)
    err = ctypes.get_last_error()
    k32.CloseHandle(f)
    k32.CloseHandle(h)
    print('ok' if ok else f'failed {err:#x}', out)


if __name__ == '__main__':
    main()
