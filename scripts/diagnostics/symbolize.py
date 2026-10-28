"""Names offsets in our DLL from its PDB (dbghelp), for dumps and stall reports.

    python symbolize.py 0x19DC68 0x56F6D4 ...      (offsets from steam_api64.dll's base)

Uses build\\x64\\Release\\steam_api64.dll and the .pdb beside it -- the build that is deployed.
"""
import ctypes
import ctypes.wintypes as wt
import os
import sys

DLL = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'build', 'x64', 'Release', 'steam_api64.dll')
BASE = 0x10000000


class SYMBOL_INFO(ctypes.Structure):
    _fields_ = [('SizeOfStruct', wt.ULONG), ('TypeIndex', wt.ULONG), ('Reserved', ctypes.c_ulonglong * 2),
                ('Index', wt.ULONG), ('Size', wt.ULONG), ('ModBase', ctypes.c_ulonglong), ('Flags', wt.ULONG),
                ('Value', ctypes.c_ulonglong), ('Address', ctypes.c_ulonglong), ('Register', wt.ULONG),
                ('Scope', wt.ULONG), ('Tag', wt.ULONG), ('NameLen', wt.ULONG), ('MaxNameLen', wt.ULONG),
                ('Name', ctypes.c_char * 1024)]


class LINE(ctypes.Structure):
    _fields_ = [('SizeOfStruct', wt.DWORD), ('Key', ctypes.c_void_p), ('LineNumber', wt.DWORD),
                ('FileName', ctypes.c_char_p), ('Address', ctypes.c_ulonglong)]


def main():
    d = ctypes.WinDLL('dbghelp')
    h = wt.HANDLE(0x1234)
    d.SymSetOptions(0x2 | 0x10)  # undecorate, load lines
    d.SymInitialize(h, os.path.dirname(os.path.abspath(DLL)).encode(), False)
    d.SymLoadModuleEx.restype = ctypes.c_ulonglong
    base = d.SymLoadModuleEx(h, None, os.path.abspath(DLL).encode(), None, ctypes.c_ulonglong(BASE), 0, None, 0)
    if not base:
        sys.exit('could not load the module')
    for a in sys.argv[1:]:
        off = int(a, 16)
        si = SYMBOL_INFO()
        si.SizeOfStruct = ctypes.sizeof(SYMBOL_INFO) - 1024
        si.MaxNameLen = 1024
        disp = ctypes.c_ulonglong(0)
        name = '?'
        if d.SymFromAddr(h, ctypes.c_ulonglong(BASE + off), ctypes.byref(disp), ctypes.byref(si)):
            name = f'{si.Name.decode(errors="replace")}+0x{disp.value:X}'
        ln = LINE()
        ln.SizeOfStruct = ctypes.sizeof(LINE)
        d2 = wt.DWORD(0)
        where = ''
        if d.SymGetLineFromAddr64(h, ctypes.c_ulonglong(BASE + off), ctypes.byref(d2), ctypes.byref(ln)):
            where = f'  {os.path.basename(ln.FileName.decode(errors="replace"))}:{ln.LineNumber}'
        print(f'0x{off:X}  {name}{where}')


if __name__ == '__main__':
    main()
