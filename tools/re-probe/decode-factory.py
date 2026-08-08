#!/usr/bin/env python3
"""Decode Fable's UI component factory jump table into a type catalogue."""
import struct
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

EXE = ("/Users/jeremypotter/Library/Containers/com.isaacmarovitz.Whisky/Bottles/"
       "3230151B-BEA6-48C5-8C7D-A7580586BF68/drive_c/Games/Fable/Fable.exe")
pe = pefile.PE(EXE, fast_load=True)
BASE = pe.OPTIONAL_HEADER.ImageBase
DATA = pe.get_memory_mapped_image()
md = Cs(CS_ARCH_X86, CS_MODE_32)

JUMPTAB = 0x0041D7F8
NTYPES = 0x2B + 1
GAME_MALLOC = 0x00BFEA1A

# --- map every vtable to a class name via RTTI -------------------------------
def dwords_equal(v):
    n = struct.pack("<I", v)
    out, off = [], 0
    while True:
        i = DATA.find(n, off)
        if i < 0:
            return out
        out.append(BASE + i)
        off = i + 1

vt_to_name = {}
off = 0
while True:
    i = DATA.find(b".?AV", off)
    if i < 0:
        break
    off = i + 1
    end = DATA.find(b"\x00", i)
    if end < 0 or end - i > 120:
        continue
    raw = DATA[i:end].decode("ascii", "replace")
    td = BASE + i - 8
    for ref in dwords_equal(td):
        col = ref - 12
        o = col - BASE
        if o < 0 or o + 4 > len(DATA):
            continue
        if struct.unpack_from("<I", DATA, o)[0] != 0:
            continue
        for r in dwords_equal(col):
            vt_to_name.setdefault(r + 4, raw)

def clean(n):
    return n.replace(".?AV", "").replace("@NUISystem@@", " (NUISystem)").replace("@@", "")

# --- decode each jump-table case ---------------------------------------------
print(f"{'type':>4}  {'size':>6}  {'ctor':<12} class")
print("-" * 74)
rows = []
for t in range(NTYPES):
    tgt = struct.unpack_from("<I", DATA, JUMPTAB - BASE + t * 4)[0]
    size = None
    ctor = None
    o = tgt - BASE
    for ins in md.disasm(DATA[o:o + 0x40], tgt):
        if ins.mnemonic == "push" and ins.op_str.startswith("0x") and size is None:
            try:
                size = int(ins.op_str, 16)
            except ValueError:
                pass
        if ins.mnemonic == "call":
            try:
                a = int(ins.op_str, 16)
            except ValueError:
                continue
            if a == GAME_MALLOC:
                continue
            ctor = a
            break
    # name the class: find the vtable this ctor writes
    name = "?"
    if ctor:
        co = ctor - BASE
        for ins in md.disasm(DATA[co:co + 0x60], ctor):
            if ins.mnemonic == "mov" and "dword ptr [e" in ins.op_str and ", 0x1" in ins.op_str:
                try:
                    v = int(ins.op_str.split(", ")[-1], 16)
                except ValueError:
                    continue
                if v in vt_to_name:
                    name = clean(vt_to_name[v])
                    break
    rows.append((t, size, ctor, name))
    print(f"0x{t:02X}  {size if size else 0:6}  0x{ctor:08X}   {name}" if ctor
          else f"0x{t:02X}  {'':6}  {'-':<12} (no ctor / default case)")
