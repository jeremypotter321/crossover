#!/usr/bin/env python3
"""Dump the script-command vocabulary: `push imm32` where imm32 is a string VA,
inside a given code range. Fable's dispatcher is a strncmp chain of these."""
import struct, sys
import pefile

EXE = ("/Users/jeremypotter/Library/Containers/com.isaacmarovitz.Whisky/Bottles/"
       "3230151B-BEA6-48C5-8C7D-A7580586BF68/drive_c/Games/Fable/Fable.exe")
pe = pefile.PE(EXE, fast_load=True)
BASE = pe.OPTIONAL_HEADER.ImageBase
DATA = pe.get_memory_mapped_image()
END = BASE + len(DATA)


def read_str(va, maxlen=64):
    off = va - BASE
    if off < 0 or off >= len(DATA):
        return None
    out = bytearray()
    while off < len(DATA) and len(out) < maxlen:
        c = DATA[off]
        if c == 0:
            break
        if not (0x20 <= c < 0x7F):
            return None
        out.append(c)
        off += 1
    else:
        return None
    if len(out) < 3:
        return None
    return out.decode()


def scan(lo, hi):
    off, out = lo - BASE, []
    end = hi - BASE
    while off < end - 5:
        if DATA[off] == 0x68:  # push imm32
            v = struct.unpack_from("<I", DATA, off + 1)[0]
            if BASE <= v < END:
                s = read_str(v)
                if s and s[0].isalpha():
                    out.append((BASE + off, v, s))
        off += 1
    return out


if __name__ == "__main__":
    lo = int(sys.argv[1], 16)
    hi = int(sys.argv[2], 16)
    pat = sys.argv[3].lower() if len(sys.argv) > 3 else None
    for va, sva, s in scan(lo, hi):
        if pat and pat not in s.lower():
            continue
        print(f"0x{va:08X}  push 0x{sva:08X}  {s}")
