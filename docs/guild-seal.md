# The guild seal

How Fable gives the hero the guild seal, how the seal UI decides whether to show, and how
`tools/re-probe/seal.c` reproduces it from an injected DLL.

All addresses are absolute in `Fable.exe` (`ImageBase = 0x400000`, no ASLR). Everything
marked **verified** was either observed live in `probe.log` or read directly out of the
image; nothing here is inferred from a disassembler's guess at a function boundary.

---

## 1. Correction to the previous handoff

The old handoff listed `0x0058EEEC` as *"function that resolves the seal by name — contains
the whole give sequence, takes the recipient at `[ebp+8]`"*. That is wrong and cost time:

- `0x0058EEEC` is a `jmp` in the **middle of an unrelated function** that starts at
  `0x0058EEC6`. It is not an entry point and takes no arguments.
- The function the handoff meant starts at **`0x0058EF79`** (`push ebp; mov ebp,esp`), and
  it does *not* give anything. It resolves the seal's definition id, looks it up in a
  `std::map` passed at `[ebp+8]`, and calls `0x0058A0C7` for that entry and then for every
  other entry in the map. It lives in the front-end/UI code block, so it is UI population,
  not inventory.

The give path is somewhere else entirely — see below.

---

## 2. The give path

`GiveHero` is a **script command**. Its dispatcher is a `strncmp` chain; the arm that
matches `"GiveHero"` is at `0x00CC6390` and its body is inline. Stripped of argument
parsing, the operative part is at `0x00CC64C5`:

```asm
push dword ptr [ebp-0x25c]     ; arg3  (bool, default 0)
mov  ecx, dword ptr [0x143E8F8]; the CGameScriptInterface singleton
push dword ptr [ebp+0xc]       ; arg2  (int,  default -1)
mov  eax, dword ptr [ecx]      ; vtable
lea  edx, [ebp-0x50]           ; arg1  CharString of the object name
push edx
call dword ptr [eax + 0x1e4]   ; <-- GiveHeroObject
dec  ebx                       ; repeated `count` times (default 1)
jne  0x00CC64C5
```

So one call gives one object, by **name**, and the whole thing is reachable from two
globals and a vtable slot:

| What | Where | Notes |
| --- | --- | --- |
| `CGameScriptInterface *` | `*(void **)0x0143E8F8` | also mirrored at `0x0143E8F0` |
| its vtable | `0x01260F0C` | **verified live**; RTTI name `.?AVCGameScriptInterface@@` |
| `GiveHeroObject` | `vtable + 0x1E4` → `0x008902E0` | `__thiscall(CharString *, int, int)`, `ret 0xC` |
| definition manager | `gsi + 0x10` | |
| world | `gsi + 0x14` | |

The singleton is constructed at `0x006E7740`, which is also where the vtable constant and
both globals come from.

### Inside `GiveHeroObject` (`0x008902E0`)

```
defId = 0x009AD410(gsi->defmgr, name)        ; name -> definition id; <= 0 means unknown
if (defId <= 0) return
hero  = 0x00487DC0(0x00449970(gsi->world))   ; the hero CThing *
if (!hero || (hero[0x91] & 1)) return
0x006AC200(hero, defId, arg2, arg3, CharString(""))   ; the actual give
```

`arg2` is tested inside `0x006AC200` and must be non-zero — both the script command and the
story quest pass `-1`. `arg3` is a *silent* flag consumed back in `0x008902E0`: when it is
zero the function additionally calls `gsi->vtable[0x1C]` twice (a UI refresh); when it is
one it skips them.

### What the story itself does

The seal is handed over by **`Q_GuildTrainingDeparture`**, whose code is compiled into the
exe. At `0x00D489B5`:

```asm
push -1
push 0x1251B4C                 ; "OBJECT_GUILD_SEAL_1"
lea  ecx, [esp+0x1c]
call 0x0099EBF0                ; CharString ctor
mov  ecx, dword ptr [esi+0x40] ; the script interface
mov  edx, dword ptr [ecx]
push 1                         ; arg3
push -1                        ; arg2
lea  eax, [esp+0x1c]
push eax                       ; arg1
call dword ptr [edx+0x1e4]     ; the same GiveHeroObject
```

i.e. `GiveHeroObject("OBJECT_GUILD_SEAL_1", -1, 1)`. The probe now passes exactly this.
Immediately after, the quest teleports to `HeroGuildComplexInsideHSP` and gives
`OBJECT_HERO_BOOTS` / `_TROUSERS` / `_SHIRT` / `_GLOVES` — so in the shipped game the seal
arrives together with the apprentice outfit, not on its own.

---

## 3. Where the seal has to *be* for the UI to see it

This is the part that matters for the menu, and it is not "anywhere in the inventory".

A `CThing` carries its inventory as **numbered container slots**:

| Field | Meaning | Accessor |
| --- | --- | --- |
| `thing + 0x20` | bitset of the container slots this thing owns | `0x00410DE0(&bitset, slot) -> bool` |
| `thing + 0x44` | `std::map<int slot, container *>` | `0x004365B0(&map, &slot) -> pair *` |
| — | how many of a definition a container holds | `0x005BDF08(container, defId) -> int` |

The map is a stock MSVC 7.1 tree: the returned `pair *` has the key at `+0` and the value
at `+4`, and "not found" is `result == *(void **)(map + 4)` (the head node) or
`*(int *)result > key`.

The HUD's quest orb (`HUD_ORB_QUEST_CORE`, function starting `0x0064A3AC`) gates itself on
the seal like this, at `0x0064A75C`:

```asm
mov  ecx, dword ptr [ebp-4]
push 0x11                      ; <-- container slot 0x11
add  ecx, 0x20
call 0x00410DE0                ; does the hero own slot 0x11 at all?
test al, al
je   <bail>                    ; if not, nothing seal-related is drawn
...                            ; else find slot 0x11 and count OBJECT_GUILD_SEAL_1 in it
call 0x005BDF08
test eax, eax
jle  <bail>
mov  byte ptr [esi+0x18], 1    ; "hero has the seal"
```

**Slot `0x11` is the one the seal UI reads.** Note the ownership test comes *first*: a hero
that does not own slot `0x11` fails before the container is ever consulted, so giving the
object can succeed and still leave the UI seeing nothing.

The give primitive `0x006AC200` picks its destination slot from data rather than being told
one: it reads container slot `0x1A` off the *object definition*, derives a slot index from
it (`0x005D9030`), tests that bit in the hero's own `+0x20` bitset, and only then resolves
the hero's container for that slot and inserts via `0x005D8D50`. So the destination depends
on both the definition and on which slots the hero owns.

---

## 4. Verified live

From `probe.log`, with the probe injected at process start and the game loaded to a hero:

```
t=41s  CGameScriptInterface @0x0899F490  vtable 0x01260F0C
        defmgr(+0x10)=0x055E6450  world(+0x14)=0x05613040
t=41s  HERO @0x08C888B8
  CharString("OBJECT_GUILD_SEAL_1") -> 0x0577E678
  definition id = 4305
  calling GiveHeroObject @0x008902E0 ...
  returned without faulting
```

- The vtable read back as `0x01260F0C`, exactly the constant in the constructor — the
  singleton and the whole route are correct.
- `OBJECT_GUILD_SEAL_1` resolves to **definition id 4305**.
- The call runs on the injected thread without faulting. (It is still an off-thread call
  into the game's inventory code; if it ever does fault, move it onto the game thread with
  the INT3 + vectored-handler technique rather than an inline hook.)

Not yet established: whether the object actually lands in slot `0x11`, and whether the
tutorial hero owns that slot at all. `dump_state()` in the probe reports both either side
of the give.

---

## 5. Names and ids

`data/CompiledDefs/names.bin` (see `ui-system.md` for its format) carries the seal family:

| Name | Role |
| --- | --- |
| `OBJECT_GUILD_SEAL_1` | the inventory object — **def id 4305** |
| `OBJECT_GUILDSEAL_BASE`, `OBJECT_GUILDSEAL_FULL` | the physical meshes |
| `CS_GUILD_SEAL_GIVE`, `CS_GUILD_SEAL_RECEIVE` | the hand-over cutscenes |
| `GUILD_SEAL_CHARGEUP` | referenced at `0x0064DA07` |

The seal menu's own screens are the `UI_QUESTS_*` / `UI_MAP_*` / `UI_STATS_*` /
`UI_LOGBOOK_*` families, plus the `UI_*_BUTTON` tabs.

`GAME_ACTION_CHARGE_GUILD_SEAL` and the `GAME_ACTION_OPEN_*_MENU` values in
`Core/SDK/Fable/GameAction.h` are the input actions; the enum names are not strings in the
binary, so they cannot be located by search.

---

## 6. Useful addresses collected here

| Address | Signature | What |
| --- | --- | --- |
| `0x008902E0` | `__thiscall(gsi, CharString *, int, int)` | `GiveHeroObject` (`vtable+0x1E4`) |
| `0x006AC200` | `__thiscall(hero, int defId, int, int, CharString *)` | the give primitive |
| `0x009AD410` | `__thiscall(defmgr, CharString *) -> int` | name → definition id |
| `0x00449970` | `__thiscall(gsi+0x14) -> world` | |
| `0x00487DC0` | `__thiscall(world) -> CThing *` | the hero |
| `0x00410DE0` | `__thiscall(thing+0x20, int slot) -> bool` | owns container slot |
| `0x004365B0` | `__thiscall(thing+0x44, int *slot) -> pair *` | find container slot |
| `0x005BDF08` | `__thiscall(container, int defId) -> int` | count of a definition |
| `0x0099EBF0` / `0x0099EAE0` | `__thiscall(CharString *, const char *, int)` / `(CharString *)` | ctor / dtor, 4 bytes wide |
| `0x00CC6390` | — | the `GiveHero` script-command arm |
| `0x00D489B5` | — | `Q_GuildTrainingDeparture` giving the seal |
| `0x0064A3AC` | — | `HUD_ORB_QUEST_CORE`, gates on slot `0x11` |
| `vtable+0x594` | `__thiscall(gsi, bool)` | the `HUD` script command's slot |

---

## 7. Tooling

`tools/re-static/` holds the static-analysis scripts this write-up was produced with
(`re_tool.py` xrefs/disasm, `re2.py` int3-based function boundaries and caller search,
`strfind.py` string search with xrefs, `cmds.py` script-command vocabulary dump). They need
a Python 3.8 venv with `capstone` and `pefile`; they were previously kept in a scratch
directory and were nearly lost.
