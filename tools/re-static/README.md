# Static analysis helpers for `Fable.exe`

These were living in a session scratch directory and were nearly lost. They read the game
executable straight off disk (path is hardcoded to the Whisky bottle at the top of each
script) and need Python with `capstone` and `pefile`:

```sh
python3 -m venv re-venv
./re-venv/bin/pip install capstone pefile
```

| Script | What it does |
| --- | --- |
| `re_tool.py xref <hex> [all]` | every 4-byte reference to a constant, with a guessed containing function |
| `re_tool.py dis <hex> [len]` | linear disassembly from an address |
| `re_tool.py func <hex> [len]` | walk back to a `push ebp; mov ebp,esp` prologue, then disassemble |
| `re2.py func <hex> [len]` | better boundaries — MSVC pads with `int3`, so walk back to the last padding run |
| `re2.py callers <hex>` | every direct `call rel32` targeting an address |
| `strfind.py <substr>...` | NUL-terminated strings containing a substring, with their xrefs |
| `cmds.py <lo> <hi> [filter]` | every `push imm32` in a range where the immediate is a string — this is how the script-command vocabulary was recovered |
| `rtti.py name/vt/slots` | MSVC RTTI: class name ↔ vtable, and vtable slot dumps |
| `console-cmds.py [substr]` | the whole dev-console table — 20 commands with their handler addresses, 22 variables with their storage addresses |

`re2.py func` is the one to trust. `re_tool.py func` guesses prologues and will happily
start mid-function; both were wrong about `0x0058EEEC` in an earlier pass, which is what
put a bogus address into the handoff.

Two habits worth keeping:

- **Check the disassembly is aligned.** Jump tables and inline data make linear disassembly
  drift. If you see `ljmp`, `aaa`, `out dx, eax` or similar nonsense, you started at the
  wrong offset — re-anchor on a known prologue.
- **Confirm a function boundary before quoting an address in docs.** An address that lands
  mid-function reads exactly like an entry point in a table.
