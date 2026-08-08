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

## The main-menu entry point

`sub_59899A` — one argument, `this` in `ecx`. This is the function that opens the main menu.

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

1. Identify the concrete type of `this` in `sub_59899A` (expected `CFrontEndManager` or
   `CNewFrontendGameComponent`) by cross-referencing its vtable against the table above.
2. Find where a `CFrontEndScreen` populates its children, and the method that appends a
   `CFrontEndButton`. Start from the button constructor sites in `0x54DE00`–`0x54E500` and
   walk callers.
3. Hook the screen-construction path, let the original run, then append one extra button —
   cloning an existing button is far cheaper than constructing one from scratch, since it
   avoids having to reproduce full element initialisation.

This cannot be validated without building the DLL, which requires the Windows CI job — the
hooks are address-based and need write/build/inject/observe iteration to get right.
