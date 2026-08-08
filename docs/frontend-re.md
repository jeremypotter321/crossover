# Frontend reverse engineering notes

Working notes toward hooking Fable's main menu (adding menu entries). Every address here
was verified against the Steam build of `Fable.exe` (appid 204030, depot 204031,
16,666,624 bytes).

## Ground rules

`Fable.exe` is a 32-bit PE with **ASLR disabled** and `ImageBase = 0x400000`, so it always
loads at its preferred base. This is why the existing hooks in `Core/SDK/` can use absolute
addresses (`0xBFEA1A`, `0x13B8650`) with no rebasing. Every address below is absolute and
valid at runtime as-is.

Sections:

| Section | VA | Virtual size |
| --- | --- | --- |
| `.text` | `0x00401000` | `0xE2B8A8` |
| `.rdata` | `0x0122D000` | `0x14684C` |
| `.data` | `0x01374000` | `0xCA9A4` |

## Frontend classes (from MSVC RTTI)

The UI lives in the `NUISystem` namespace. Primary vtables:

| Class | Type descriptor | Primary vtable | Virtual methods |
| --- | --- | --- | --- |
| `NUISystem::CFrontEndManager` | `0x0137CC80` | `0x012521A8` | 9 |
| `NUISystem::CFrontEndScreen` | `0x0137C178` | `0x012497E4` | 143 |
| `NUISystem::CFrontEndButton` | `0x0137C120` | `0x01249554` | 151 |
| `NUISystem::CFrontEndList` | `0x0137C09C` | `0x01249224` | 190 |
| `CFrontEndDef` | `0x01375B8C` | `0x01230FBC` | 25 |
| `CFrontendGameComponent` | `0x013787AC` | `0x01238768` | 5 |
| `CNewFrontendGameComponent` | `0x01375AD4` | `0x01230CA0` | 5 |

Sites writing these vtables (i.e. constructors / destructors) cluster in `0x54DE00`–`0x54E500`:

- `CFrontEndButton` vtable written at `0x0054DEDC`, `0x0054E0C1`, `0x0054E131`
- `CFrontEndScreen` vtable written at `0x0054E3E1`, `0x0054E421`, `0x0054E455`

## Menu definitions

Screens are referenced by **definition name**, not built inline. Names live in
`data/CompiledDefs/names.bin`, which is a flat table of `[4-byte hash][NUL-terminated name]`
records behind a 16-byte header (record count `0x3519` = 13593, payload size `0x60E68`).
It contains 289 `UI_FRONTEND*` entries, including four main-menu variants.

String literals referenced from code:

| VA | Definition |
| --- | --- |
| `0x01252374` | `UI_FRONTEND_MAIN_MENU_NO_LIVEAWARE_NO_CONTINUE` |
| `0x012524E4` | `UI_FRONTEND_MAIN_MENU_NO_LIVEAWARE` |
| `0x01252878` | `UI_FRONTEND_EXTRAS_MENU` |
| `0x012528B0` | `UI_FRONTEND_OPTIONS_MENU` |

> **Dead end — do not repeat.** The 4-byte hash preceding a name in `names.bin` appears
> **zero** times in `game.bin`, `frontend.bin` and `script.bin`. Definition payloads are not
> keyed by that hash in any compiled def file, so there is no cheap way to find and edit a
> screen's child-element list as data. `frontend.bin` is the sprite/graphic bank (see
> `data/Defs/RetailHeaders/pc/front_end_bank.h`), not layout. Editing defs would mean
> reverse engineering an undocumented binary schema; hooking code is the cheaper route.

## `CFrontEndManager`'s code block

Its constructor/destructor writes the vtable at `0x005953F1` and `0x005954B6`, both inside
one function starting at **`0x00595356`**. MSVC emits a class's methods contiguously, so the
run from roughly `0x595356` to `0x59C000` is `CFrontEndManager`. That places both menu
functions below inside the class — neither is virtual, so `this` (`ecx`) is a
`CFrontEndManager*` in both.

Its 9 virtual methods:

```
[0] 0x0059B641   [1] 0x0059B5C2   [2] 0x0052D900
[3] 0x0052DA20   [4] 0x0052D940   [5] 0x0052D9A0
[6] 0x0052D7B0   [7] 0x0041C580   [8] 0x0059A238
```

## The main-menu entry point

`sub_59899A` — `CFrontEndManager` method, one argument, `this` in `ecx`. This is the
function that opens the main menu.

```
0x0059899A  push  ebp                      ; this = ecx -> esi
...
0x005989CF  call  0x597df7                 ; fills a 3-dword container at [ebp-0x10]
0x005989D4  mov   eax, [ebp-0x10]
0x005989D7  cmp   eax, [ebp-0x0c]          ; begin == end ?  (container empty?)
0x005989DF  jne   0x5989e8
0x005989E1  push  0x1252374                ; empty  -> ..._NO_LIVEAWARE_NO_CONTINUE
0x005989E6  jmp   0x5989ed
0x005989E8  push  0x12524e4                ; non-empty -> ..._NO_LIVEAWARE
0x005989ED  call  0x99ebf0                 ; CharString ctor from literal
0x005989F8  call  0x595a06                 ; <-- open screen by def name (this=esi)
0x00598A00  call  0x99eae0                 ; CharString dtor
0x00598A0A  call  0x595b24
```

The empty/non-empty container decides **Continue vs no-Continue**, which matches the
variant names — so the container at `[ebp-0x10]` is the save-game list. The PC build only
ever uses the `NO_LIVEAWARE` pair (no Xbox Live).

### Helper functions seen

| Address | Role (inferred) |
| --- | --- |
| `0x00595A06` | Open/select a frontend screen by definition name. Reads a member at `this+0x54`, then dispatches a virtual call at **vtable offset `0x150`** (index 84) to resolve the name. |
| `0x0059B5D7` | Accessor used twice inside `0x595A06`; returns a pointer to a pointer (container element). |
| `0x0099EBF0` | `CharString` construct-from-literal (`push -1` length convention). |
| `0x0099EAE0` | `CharString` destructor. |
| `0x0099E900` | String comparison — result in `al`. |
| `0x00597DF7` | Populates the save-game container. |

## Remaining work

To add a menu entry, the next steps are:

1. ~~Identify the concrete type of `this` in `sub_59899A`.~~ Done — it is
   `CFrontEndManager`, established from the constructor's location at `0x00595356`.
2. Find where a `CFrontEndScreen` populates its children, and the method that appends a
   `CFrontEndButton`. Start from the button constructor sites in `0x54DE00`–`0x54E500` and
   walk callers.
3. Hook the screen-construction path, let the original run, then append one extra button —
   cloning an existing button is far cheaper than constructing one from scratch, since it
   avoids having to reproduce full element initialisation.

This cannot be validated without building the DLL, which requires the Windows CI job — the
hooks are address-based and need write/build/inject/observe iteration to get right.

---

# Live findings (verified by injected probe)

Everything below was confirmed against the running game via `tools/re-probe`, not
inferred from static analysis.

## Object model

| Fact | Value |
| --- | --- |
| Button definition name | `*(char **)*(DWORD *)(button + 0x20)` |
| Button render position (vertical) | `float` at `button + 0x038`, 30.0 per menu row |
| `CFrontEndList` child vector | `{begin,end,capacity}` at list `+0x164 / +0x168 / +0x16C` |
| Main menu | the `CFrontEndList` whose children include `UI_FRONTEND_BUTTON_QUIT` |
| Button object size | > `0x1B0` (ctor at `0x0054DED0` touches `+0x1B0`) |
| Button vtables | `+0x00`, `+0x04`, `+0x18` — multiple inheritance |

Live main-menu children, in order: `UI_FRONTEND_BUTTON_LOAD_GAME`,
`_CHANGE_PROFILE`, `_OPTIONS`, `_CREDITS`, `_ABOUT`, `_QUIT`.

## What works

Writing `button + 0x038` on a **live** button visibly moves it: setting the Options
button from 60.0 to 160.0 moved that row to the bottom of the menu on screen. So the
renderer reads button state per frame, and buttons are individually addressable.

## What does NOT work — the `+0x164` vector is not the draw source

This was tested to destruction, and all of it is negative:

1. **Append a cloned button** (vector moved to our own storage, 7th entry, Y offset by one
   row) — menu still rendered 6 entries.
2. **Append early and repeatedly** — 226 appends starting at t≈12s, before and across the
   menu being built. Still 6 entries.
3. **Swap the last two entries** — rendered order unchanged. (Inconclusive on its own:
   buttons carry absolute positions, so list order need not affect layout.)
4. **Shrink the vector** so Quit is excluded — **Quit still rendered**. This is decisive:
   the vector does not determine what is drawn.
5. **Overwrite every pointer to the Quit button** with the Credits pointer — including
   interior pointers at `+0x04`/`+0x18` for the multiple-inheritance bases, and rescanned
   with `MEM_IMAGE`/`MEM_MAPPED` included so the exe's own `.data` was covered. Quit still
   rendered.
6. **Guard page** on the button's page with a vectored exception handler to catch the
   reading instruction — zero faults; Wine does not deliver `STATUS_GUARD_PAGE_VIOLATION`
   here, so this technique is unavailable under Wine.

A full scan for *any* pointer landing inside the Quit button found only 9, at offsets
`+0x000` (x4), `+0x004` (x2), `+0x1C8`, `+0x378`, `+0x3C0`.

**Conclusion:** the set of drawn entries is not resolved by following button pointers at
draw time. It is fixed when the screen is constructed, while per-entry properties (like
position) are still read live. Mutating containers after the fact therefore cannot add a
menu entry.

## The remaining route

Adding an entry has to happen at screen-construction time, through the game's own
machinery, rather than by patching containers afterwards:

1. From `sub_59899A` / `sub_595A06`, follow the path that turns a screen definition into
   live `CFrontEndButton` objects.
2. Find the routine that creates one button from a definition and attaches it to a screen.
3. Detour that construction path and invoke the same routine once more for an extra entry.

This needs a real detour (MinHook or hand-rolled trampoline) rather than the data pokes
used so far, since it has to run during construction rather than after it.
