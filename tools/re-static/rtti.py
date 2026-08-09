#!/usr/bin/env python3
"""
Resolve MSVC RTTI in Fable.exe: class name -> vtable, and vtable -> class name.

A naive "find the first dword that points at the type descriptor" is wrong --
it matches base-class-descriptor arrays as often as the object locator, which
is why an earlier pass found nothing for the console classes. This validates
the complete object locator properly:

    COL +0x00  signature (0 for 32-bit)
        +0x04  offset
        +0x08  cdOffset
        +0x0C  pTypeDescriptor
        +0x10  pClassDescriptor

and a vtable is any dword pointing at a COL, with the vtable starting 4 bytes
after it.

  rtti.py name <substring>     classes whose name contains <substring>
  rtti.py vt <hex>             which class a vtable belongs to
  rtti.py slots <hex> [n]      dump n vtable slots
"""
import struct, sys
import pefile

EXE = ("/Users/jeremypotter/Library/Containers/com.isaacmarovitz.Whisky/Bottles/"
       "3230151B-BEA6-48C5-8C7D-A7580586BF68/drive_c/Games/Fable/Fable.exe")
pe = pefile.PE(EXE, fast_load=True)
BASE = pe.OPTIONAL_HEADER.ImageBase
DATA = pe.get_memory_mapped_image()
END = BASE + len(DATA)


def dw(va):
    return struct.unpack_from("<I", DATA, va - BASE)[0]


def cstr(va, maxlen=200):
    o = va - BASE
    e = DATA.find(b"\0", o)
    return DATA[o:e].decode("ascii", "replace")[:maxlen]


def type_descriptors():
    """Every RTTI type descriptor, as (td_va, decorated_name)."""
    off = 0
    while True:
        i = DATA.find(b".?AV", off)
        if i < 0:
            return
        yield BASE + i - 8, cstr(BASE + i)
        off = i + 1


def col_for(td):
    """Complete object locators whose pTypeDescriptor is td, validated."""
    needle = struct.pack("<I", td)
    off, out = 0, []
    while True:
        i = DATA.find(needle, off)
        if i < 0:
            return out
        col = BASE + i - 0x0C
        try:
            if dw(col) == 0 and dw(col + 0x10) > BASE:
                out.append(col)
        except Exception:
            pass
        off = i + 1


def vtables_for(col):
    needle = struct.pack("<I", col)
    off, out = 0, []
    while True:
        i = DATA.find(needle, off)
        if i < 0:
            return out
        out.append(BASE + i + 4)
        off = i + 1


def by_name(sub):
    for td, name in type_descriptors():
        if sub.lower() not in name.lower():
            continue
        for col in col_for(td):
            for vt in vtables_for(col):
                print(f"{name}\n    TD 0x{td:08X}  COL 0x{col:08X}  vtable 0x{vt:08X}")


if __name__ == "__main__":
    cmd = sys.argv[1]
    if cmd == "name":
        by_name(sys.argv[2])
    elif cmd == "vt":
        vt = int(sys.argv[2], 16)
        col = dw(vt - 4)
        print(cstr(dw(col + 0x0C) + 8))
    elif cmd == "slots":
        vt = int(sys.argv[2], 16)
        n = int(sys.argv[3]) if len(sys.argv) > 3 else 16
        for k in range(n):
            print(f"  +0x{4*k:03X} -> 0x{dw(vt + 4*k):08X}")
