#!/usr/bin/env python3
"""Better function boundaries for Fable.exe: MSVC pads with int3 between functions."""
import struct, sys
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

EXE = ("/Users/jeremypotter/Library/Containers/com.isaacmarovitz.Whisky/Bottles/"
       "3230151B-BEA6-48C5-8C7D-A7580586BF68/drive_c/Games/Fable/Fable.exe")
pe = pefile.PE(EXE, fast_load=True)
BASE = pe.OPTIONAL_HEADER.ImageBase
DATA = pe.get_memory_mapped_image()
TEXT_LO, TEXT_HI = BASE + 0x1000, BASE + 0xE2C000
md = Cs(CS_ARCH_X86, CS_MODE_32)


def func_start(addr, limit=0x2000):
    """Walk back to the byte after the last int3 padding run."""
    off = addr - BASE
    for back in range(1, limit):
        i = off - back
        if i < 1:
            break
        if DATA[i - 1] == 0xCC and DATA[i] != 0xCC:
            return BASE + i
    return None


def callers(target):
    """Direct `call rel32` sites targeting `target`."""
    out = []
    for i in range(len(DATA) - 5):
        if DATA[i] != 0xE8:
            continue
        va = BASE + i
        if not (TEXT_LO <= va < TEXT_HI):
            continue
        rel = struct.unpack_from("<i", DATA, i + 1)[0]
        if va + 5 + rel == target:
            out.append(va)
    return out


def disasm(start, length=0x120, mark=None):
    off = start - BASE
    for ins in md.disasm(DATA[off:off + length], start):
        flag = "   <<<" if mark and ins.address <= mark < ins.address + ins.size else ""
        print(f"  0x{ins.address:08X}  {ins.mnemonic:<7} {ins.op_str}{flag}")


if __name__ == "__main__":
    cmd = sys.argv[1]
    if cmd == "func":
        a = int(sys.argv[2], 16)
        s = func_start(a)
        print(f"function start: 0x{s:08X}" if s else "not found")
        if s:
            disasm(s, int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x120, mark=a)
    elif cmd == "callers":
        t = int(sys.argv[2], 16)
        cs = callers(t)
        print(f"callers of 0x{t:08X}: {len(cs)}")
        for c in cs:
            s = func_start(c)
            print(f"  call at 0x{c:08X}   in function 0x{s:08X}" if s else f"  call at 0x{c:08X}")
