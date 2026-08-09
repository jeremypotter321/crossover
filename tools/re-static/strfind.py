#!/usr/bin/env python3
"""Find NUL-terminated ASCII strings in Fable.exe by substring, and their xrefs."""
import struct, sys
import pefile

EXE = ("/Users/jeremypotter/Library/Containers/com.isaacmarovitz.Whisky/Bottles/"
       "3230151B-BEA6-48C5-8C7D-A7580586BF68/drive_c/Games/Fable/Fable.exe")
pe = pefile.PE(EXE, fast_load=True)
BASE = pe.OPTIONAL_HEADER.ImageBase
DATA = pe.get_memory_mapped_image()


def find_strings(sub):
    needle = sub.encode()
    off, out = 0, []
    while True:
        i = DATA.find(needle, off)
        if i < 0:
            return out
        # walk back to start of string
        s = i
        while s > 0 and 0x20 <= DATA[s - 1] < 0x7F:
            s -= 1
        e = i
        while e < len(DATA) and 0x20 <= DATA[e] < 0x7F:
            e += 1
        if e < len(DATA) and DATA[e] == 0:
            out.append((BASE + s, DATA[s:e].decode("ascii", "replace")))
        off = i + 1


def xrefs(value):
    needle = struct.pack("<I", value)
    off, out = 0, []
    while True:
        i = DATA.find(needle, off)
        if i < 0:
            return out
        out.append(BASE + i)
        off = i + 1


if __name__ == "__main__":
    seen = set()
    for sub in sys.argv[1:]:
        print(f"### substring {sub!r}")
        for va, s in find_strings(sub):
            if va in seen:
                continue
            seen.add(va)
            refs = xrefs(va)
            print(f"  0x{va:08X}  {s!r}   refs={len(refs)} " +
                  " ".join(f"0x{r:08X}" for r in refs[:12]))
        print()
