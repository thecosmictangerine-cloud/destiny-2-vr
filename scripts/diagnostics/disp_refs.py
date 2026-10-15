"""Every instruction in the live image snapshot that uses a given displacement off a register.

    python disp_refs.py 0x1720 [0x1710 ...] [--writes|--reads]

Field offsets are how a structure's users find each other in a stripped binary: whoever reads
`[reg + 0x1720]` is a consumer of whatever lives there. Works on the snapshot `live_image.py dump`
takes; VMProtect leaves the original x86 of virtualised functions in place, so their bodies are
searchable too.
"""
import sys

from live_image import Image


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    mode = "both"
    if "--writes" in sys.argv:
        mode = "writes"
    if "--reads" in sys.argv:
        mode = "reads"
    img = Image()
    for spec in args:
        disp = int(spec, 16)
        pat = disp.to_bytes(4, "little")
        hits = {}
        d = img.data
        for name, va, vs, ch in img.code_sections():
            start = va
            while True:
                i = d.find(pat, start, va + vs)
                if i < 0:
                    break
                start = i + 1
                for back in range(2, 9):
                    ins = list(img.md.disasm(d[i - back:i + 10], i - back, count=1))
                    if not ins:
                        continue
                    insn = ins[0]
                    if insn.size < back + 4 or hex(disp) not in insn.op_str or "ptr [r" not in insn.op_str:
                        continue
                    if "rip" in insn.op_str or "rsp" in insn.op_str:
                        break
                    ops = insn.op_str
                    dest_is_mem = ops.split(",")[0].strip().endswith("]")
                    is_write = dest_is_mem and insn.mnemonic.startswith(("mov", "inc", "dec", "add", "sub", "or", "and"))
                    if mode == "writes" and not is_write:
                        break
                    if mode == "reads" and (is_write and insn.mnemonic.startswith("mov")):
                        break
                    if insn.mnemonic == "lea" and mode == "writes":
                        break
                    f = img.func_of(insn.address)
                    hits[insn.address] = (insn.mnemonic, ops, f[0] if f else 0)
                    break
        print(f"--- disp {disp:#x}: {len(hits)}")
        for a in sorted(hits):
            m, o, f = hits[a]
            print(f"  +{a:#x} {m} {o}   func +{f:#x}")


if __name__ == "__main__":
    main()
