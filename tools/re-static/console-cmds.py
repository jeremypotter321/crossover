#!/usr/bin/env python3
"""
Recover Fable's dev-console vocabulary straight out of Fable.exe.

Every console entry is built inline by the same idiom -- allocate, construct a
CharString name, store the vtable, store the payload, hand it to
`CConsole::AddCommand` (`0x009EC5E0`) on the singleton at `0x013CAA40`. So the
name, the class and the payload are all immediates sitting in `.text`, and the
whole table can be recovered without running the game, activating the console,
or patching a byte. This is what `CommandList` and `VarList` would have printed,
plus the handler and storage addresses they would not.

Three layouts, all confirmed against disassembly:

    CConsoleClasslessCommand  size 0x18  +0x04 name  +0x14 handler
    CConsoleCommand<T>        size 0x1C  +0x04 name  +0x14 owner    +0x18 handler
    CConsoleVariable          size 0x10  +0x04 name  +0x08 type     +0x0C storage

`CConsoleVariable` is the interesting one: `+0x0C` is the address of the actual
game global, so a variable can be read or written from an injected DLL with a
plain memory access -- no call onto the game thread, which is what has crashed
every live session so far.

    console-cmds.py                 everything, grouped by class
    console-cmds.py <substring>     only entries whose name contains <substring>
"""
import re
import struct
import sys

import pefile
from capstone import CS_ARCH_X86, CS_MODE_32, Cs

EXE = ("/Users/jeremypotter/Library/Containers/com.isaacmarovitz.Whisky/Bottles/"
       "3230151B-BEA6-48C5-8C7D-A7580586BF68/drive_c/Games/Fable/Fable.exe")
pe = pefile.PE(EXE, fast_load=True)
BASE = pe.OPTIONAL_HEADER.ImageBase
DATA = pe.get_memory_mapped_image()
END = BASE + len(DATA)
TEXT_LO, TEXT_HI = BASE + 0x1000, BASE + 0xE2C000
md = Cs(CS_ARCH_X86, CS_MODE_32)

CONSOLE_SINGLETON = 0x013CAA40  # holds the CConsole*; ctor 0x009ECD80
ADD_COMMAND = 0x009EC5E0        # thiscall(CConsole*, CConsoleInputBase*)

# vtable -> (class name, payload offset, payload meaning). Resolved from RTTI
# with rtti.py. The base-class vtables are stored too, on the way up through the
# constructors, so the most derived one seen before the payload store wins.
CLASSES = {
    0x0122E65C: ("CConsoleClasslessCommand", 0x14, "handler"),
    0x0129C4E0: ("CConsoleCommand<CConsole>", 0x18, "handler"),
    0x0125D668: ("CConsoleCommand<CGameTimeManager>", 0x18, "handler"),
    0x0122E5C8: ("CConsoleVariable", 0x0C, "storage"),
}
BASE_VTABLES = {
    0x0122E5B0: "CConsoleInputBase",
    0x0122E638: "CConsoleCommandBase",
}

NAME_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_ .]{1,63}$")
STORE_RE = re.compile(
    r"^dword ptr \[e[a-z]{2}(?: \+ (0x[0-9a-f]+))?\], (0x[0-9a-f]+|-?\d+)$")


def cstr(va, maxlen=80):
    """The NUL-terminated ASCII string at `va`, or None if it is not one."""
    if not (BASE <= va < END):
        return None
    off = va - BASE
    end = DATA.find(b"\0", off, off + maxlen)
    if end <= off:
        return None
    try:
        s = DATA[off:end].decode("ascii")
    except UnicodeDecodeError:
        return None
    return s if s.isprintable() else None


def occurrences(value):
    """Every code-section address holding `value` as a little-endian dword."""
    needle = struct.pack("<I", value)
    out, off = [], 0
    while True:
        i = DATA.find(needle, off)
        if i < 0:
            return out
        if TEXT_LO <= BASE + i < TEXT_HI:
            out.append(BASE + i)
        off = i + 1


def func_start(addr, limit=0x3000):
    """The byte after the last int3 padding run -- MSVC's function boundary."""
    off = addr - BASE
    for back in range(1, limit):
        i = off - back
        if i < 1:
            return None
        if DATA[i - 1] == 0xCC and DATA[i] != 0xCC:
            return BASE + i
    return None


def store_imm(ins):
    """`mov dword ptr [reg+disp], imm32` -> (disp, imm), else None."""
    if ins.mnemonic != "mov":
        return None
    m = STORE_RE.match(ins.op_str)
    if not m:
        return None
    disp = int(m.group(1), 16) if m.group(1) else 0
    raw = m.group(2)
    imm = int(raw, 16) if raw.startswith("0x") else int(raw) & 0xFFFFFFFF
    return disp, imm


def scan_function(start, stop):
    """Pair up (name push -> vtable store -> payload store) across one function."""
    out = []
    name = vt = at = None
    for ins in md.disasm(DATA[start - BASE:stop - BASE], start):
        if ins.mnemonic == "push" and ins.op_str.startswith("0x"):
            s = cstr(int(ins.op_str, 16))
            if s and NAME_RE.match(s):
                name, vt, at = s, None, None
            continue
        st = store_imm(ins)
        if st is None:
            continue
        disp, imm = st
        if disp == 0:
            if imm in CLASSES:
                vt, at = imm, ins.address
            elif imm not in BASE_VTABLES:
                vt = None  # some unrelated object is being built; drop the name
            continue
        if vt is not None and disp == CLASSES[vt][1]:
            out.append((name, vt, imm, at))
            name = vt = at = None
    return out


def scan():
    starts = set()
    for vt in CLASSES:
        for site in occurrences(vt):
            s = func_start(site)
            starts.add(s if s is not None and 0 < site - s < 0x3000 else site - 0x100)

    found = {}
    for s in sorted(starts):
        for name, vt, payload, at in scan_function(s, min(s + 0x4000, TEXT_HI)):
            if name and name not in found:
                found[name] = (vt, payload, at)
    return found


if __name__ == "__main__":
    want = sys.argv[1].lower() if len(sys.argv) > 1 else None
    by_class = {}
    for name, (vt, payload, at) in sorted(scan().items(), key=lambda kv: kv[0].lower()):
        if want and want not in name.lower():
            continue
        by_class.setdefault(CLASSES[vt][0], []).append((name, CLASSES[vt][2], payload, at))

    total = 0
    for cls in sorted(by_class):
        rows = by_class[cls]
        total += len(rows)
        print(f"### {cls}  ({len(rows)})")
        for name, kind, payload, at in rows:
            print(f"  {name:<34} {kind} 0x{payload:08X}   registered at 0x{at:08X}")
        print()
    print(f"{total} entries")
