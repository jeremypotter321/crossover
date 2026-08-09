#!/usr/bin/env python3
"""Small RE helpers for Fable.exe: xrefs to a constant, and function disassembly."""
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


def xrefs(value, only_code=True):
    needle = struct.pack("<I", value)
    out, off = [], 0
    while True:
        i = DATA.find(needle, off)
        if i < 0:
            return out
        va = BASE + i
        if not only_code or TEXT_LO <= va < TEXT_HI:
            out.append(va)
        off = i + 1


def func_start(addr, limit=0x600):
    """Walk back to a plausible function prologue."""
    for back in range(0, limit):
        va = addr - back
        off = va - BASE
        b = DATA[off:off + 3]
        # push ebp; mov ebp,esp   |   push ebp; lea ebp,[esp-..]
        if b[:1] == b"\x55" and (b[1:3] == b"\x8b\xec" or b[1:2] == b"\x8d"):
            prev = DATA[off - 1:off]
            if prev in (b"\xcc", b"\xc3") or DATA[off - 3:off] in (b"\xc2\x04\x00",):
                return va
    return None


def disasm(start, length=0x100, mark=None):
    off = start - BASE
    for ins in md.disasm(DATA[off:off + length], start):
        flag = ""
        if mark and ins.address <= mark < ins.address + ins.size:
            flag = "   <<<"
        print(f"  0x{ins.address:08X}  {ins.mnemonic:<7} {ins.op_str}{flag}")
        if ins.mnemonic == "ret":
            break


if __name__ == "__main__":
    cmd = sys.argv[1]
    if cmd == "xref":
        val = int(sys.argv[2], 16)
        hits = xrefs(val, only_code=(len(sys.argv) < 4))
        print(f"xrefs to 0x{val:08X}: {len(hits)}")
        for h in hits:
            fs = func_start(h)
            print(f"  0x{h:08X}   (function start ~0x{fs:08X})" if fs else f"  0x{h:08X}")
    elif cmd == "dis":
        start = int(sys.argv[2], 16)
        length = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x100
        disasm(start, length)
    elif cmd == "func":
        addr = int(sys.argv[2], 16)
        fs = func_start(addr)
        print(f"function start: 0x{fs:08X}" if fs else "not found")
        if fs:
            disasm(fs, int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x200, mark=addr)
