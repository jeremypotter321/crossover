# Fable's UI system

A study of how Fable: The Lost Chapters builds its interface, aimed at one goal: creating
new UI elements that look and behave like the game's own, for the multiplayer features.

Every address is absolute and valid as-is — `Fable.exe` is a 32-bit PE with ASLR disabled
and `ImageBase = 0x400000`, so it always loads at its preferred base. Verified against the
Steam build (appid 204030, depot 204031, 16,666,624 bytes).

---

## 1. The short version

Fable's UI is a proper retained-mode widget framework in a `NUISystem` namespace — 61
classes, a component/container tree, mixin behaviours, and an observer pattern. Screens are
**not** built in code: they are **data**, compiled into definition files, and instantiated
at load time by a single factory function.

That factory is the way in. It is one function with a jump table over 44 component types,
and every case does the same three things: allocate with the game's allocator, call the
class constructor, return the component. Anything the game can build, that function can
build — including for us.

```
0x0041D21B   the component factory
0x0041D249   mov eax, [ebx+0x3C]        ; component type, read from the definition
0x0041D24C   cmp eax, 0x2B              ; 44 types, 0x00 .. 0x2B
0x0041D255   jmp [eax*4 + 0x0041D7F8]   ; jump table
   ...per case:
   push <size>                          ; per-type object size
   call 0x00BFEA1A                      ; the game's allocator
   mov ecx, eax                         ; this
   push edi                             ; single ctor argument
   call <constructor>
```

`0x00BFEA1A` is the game's allocator — and the mod **already hooks it** (`SDK.cpp`:
`ADD_HOOK(0xBFEA1A, HGameMalloc, OGameMalloc)`), so allocation for new components can go
through the same path the game uses.

---

## 2. The widget vocabulary

61 classes live in `NUISystem`. Grouped by role:

**Core tree**
| Class | Primary vtable | vfuncs |
| --- | --- | --- |
| `CComponent` | `0x0124608C` | 134 |
| `CComponentContainer` | `0x01247AF4` | 144 |
| `CManager` | — | — |

**Frontend (menus)**
| Class | Primary vtable | vfuncs |
| --- | --- | --- |
| `CFrontEndManager` | `0x012521A8` | 9 |
| `CFrontEndScreen` | `0x012497E4` | 143 |
| `CFrontEndButton` | `0x01249554` | 151 |
| `CFrontEndList` | `0x01249224` | 190 |

**Content widgets** — `CText`, `CTextContainer`, `CIconText`, `CSprite`, `CMorphingSprite`,
`CMesh`, `CMovie`, `CParticleEffect`, `CTable`, `CViewport`

**Lists and scrolling** — `CList`, `CDynamicList`, `CScrollableList`, `CListArrow`,
`CScrollBar`, `CScrollBarOutside`, `CScrollingComponent`, `CScrollingViewport`,
`CZoomingComponent`

**Input widgets** — `CSlider`, `CTextSlider`, `CEditBox`, `CNavButton`, `CMenuEntry`,
`CKeyRedefiner`, `CRedefinerList`, `CIMEFont`

**Behaviour mixins** — `CClickable`, `CHoverable`, `CDraggable`, `CDraggableInto`,
`CObservable`, `CObserver`, `CChangingStateComponent`, `CSwappingStateComponent`

**Prefab dialogs** — `CYesNo`, `COk`, `CControllerDisconnect`

**Game-specific** — `CSpellContainer`, `CSpellContainerList`, `CMouseCursor`

**Definitions** — `CUIDef`, `CUIStateDef`, `CUIIconsDef`, `CUIMiscThingsDef`,
`CUILocaleGraphicsDef`

**Action parameters** — `CActionParam` plus typed variants `CActionParamEBOOL`,
`CActionParamFloat`, `CActionParamSLONG`, `CActionParamState`, `CActionParamString`,
`CActionParamULONG`. This is how a widget's action is parameterised from data — the hook
point for wiring a button to multiplayer code.

---

## 3. The component type catalogue

The complete jump table, decoded. **`type` is the value at `def+0x3C`**; `size` is what to
allocate; `ctor` takes `this` in `ecx` and one stack argument.

| type | size | constructor | class |
| ---: | ---: | --- | --- |
| `0x00` | 388 | `0x0041B800` | `CSprite` |
| `0x01` | 412 | `0x005545D0` | `CMorphingSprite` |
| `0x02` | 368 | `0x005517E0` | `CTable` |
| `0x03` | 420 | `0x00550190` | `CMesh` |
| `0x04` | 308 | `0x005334A0` | `CUIInterface` |
| `0x05` | 348 | `0x0052CC50` | `CChangingStateComponent` |
| `0x06` | 396 | `0x0054F5C0` | `CText` |
| `0x07` | 444 | `0x0053DFE0` | `CMenuEntry` |
| `0x08` | 508 | `0x0053B63E` | `CList` |
| `0x09` | 372 | `0x0054EA00` | `CViewport` |
| `0x0A` | 364 | `0x0054E3D0` | `CFrontEndScreen` |
| `0x0B` | 436 | `0x0054E0B0` | `CFrontEndButton` |
| `0x0C` | 508 | `0x0054C3A0` | `CFrontEndList` |
| `0x0D` | 412 | `0x0053F120` | `CScrollingViewport` |
| `0x0E` | 400 | `0x0054C1D0` | `CListArrow` |
| `0x0F` | 492 | `0x0054C050` | `CSlider` |
| `0x10` | 416 | `0x00549F60` | `CTextSlider` |
| `0x11` | 408 | `0x005482D0` | `CMovie` |
| `0x12` | 368 | `0x00547600` | `CSwappingStateComponent` |
| `0x13` | 348 | `0x00546F40` | `CScrollingComponent` |
| `0x14` | 364 | `0x00546D30` | `CTextContainer` |
| `0x15` | 388 | `0x00546B00` | `CZoomingComponent` |
| `0x16` | 348 | `0x005460C0` | `CComponentContainer` |
| `0x17` | 380 | `0x00545720` | `CSpellContainer` |
| `0x18` | 424 | `0x00544B70` | `CSpellContainerList` |
| `0x19` | 356 | `0x0041CADC` | `CYesNo` |
| `0x1A` | 352 | `0x0041CB70` | `COk` |
| `0x1B` | 356 | `0x00544010` | `CParticleEffect` |
| `0x1C` | 352 | `0x0041CBE4` | `CControllerDisconnect` |
| `0x1D` | — | `0x0044C6B0` | (fallback — `CGameDefinitionManager` accessor) |
| `0x1E` | 436 | `0x00542330` | `CIconText` |
| `0x1F` | 384 | `0x005415F0` | `CDynamicList` |
| `0x20` | 388 | `0x0055C650` | `CMouseCursor` |
| `0x21` | 364 | `0x0055BA20` | `CHoverable` |
| `0x22` | 404 | `0x0055B460` | `CClickable` |
| `0x23` | 428 | `0x0055A9C0` | `CDraggable` |
| `0x24` | 368 | `0x00558EC0` | `CDraggableInto` |
| `0x25` | 396 | `0x005407B0` | `CEditBox` |
| `0x26` | 404 | `0x00558B90` | `CNavButton` |
| `0x27` | 448 | `0x00558540` | `CKeyRedefiner` |
| `0x28` | 400 | `0x00556350` | `CRedefinerList` |
| `0x29` | 476 | `0x00559830` | `CScrollBar` |
| `0x2A` | 416 | `0x00559360` | `CScrollBarOutside` |
| `0x2B` | 380 | `0x00555180` | `CScrollableList` |

Note `CClickable`/`CHoverable`/`CDraggable` are constructible component types in their own
right, not just base classes — behaviour is composed, not inherited, at the data level.

---

## 4. The definition system

### `names.bin` — the global name table

`data/CompiledDefs/names.bin`, 396,920 bytes:

```
struct { u32 unknown; u32 magic /* 0xA8E36C34 */; u32 count /* 13593 */; u32 payload /* 0x60E68 */; u32 zero; };
then count x: { u32 name_hash; char name[]; /* NUL-terminated */ }
```

Every definition in the game is named here, and referenced at runtime by its **hash**, not
its name. `UI_FRONTEND_MAIN_MENU` hashes to `0xD8141E99`. Names come in `NULLDEF_X` / `X`
pairs — each definition has a null/default counterpart.

UI definitions by family: `UI_FRONTEND` 194, `UI_TEXT` 97, `UI_OPTIONS` 75, `UI_SPRITE` 58,
`UI_MENU` 54, `UI_TABLE` 53, `UI_RING` 34, `UI_TRADING` 31, `UI_SCREEN` 31, `UI_TITLE` 28,
`UI_STATS` 23, `UI_QUEST` 22, `UI_BACKDROP` 22, `UI_CREDITS` 20, `UI_MAP` 16, `UI_DIALOG`
15, `UI_SCOREBOARD` 14, `UI_LOGBOOK` 14.

### The compiled definition containers

`data/CompiledDefs/{game,frontend,script}.bin` share a header shape — a leading zero byte,
a 4-byte checksum, the same magic `0xA8E36C34`, then a definition count:

| file | size | definitions |
| --- | ---: | ---: |
| `game.bin` | 996,375 | 14,761 |
| `frontend.bin` | 69,662 | 810 |
| `script.bin` | 154,496 | 611 |

> **Dead end, do not repeat.** A definition's *name hash* does not appear anywhere in these
> payload files — definitions are not keyed by it on disk. Editing a screen's child list as
> data would mean fully reversing this container format first. Constructing components at
> runtime via the factory is far cheaper and is the route this project takes.

`frontend.bin` is a graphic/sprite bank, not layout — see
`data/Defs/RetailHeaders/pc/front_end_bank.h` for its `EFrontEndGraphicBank` ids.

### Definition field known so far

| offset | meaning |
| --- | --- |
| `def+0x3C` | component type (index into the catalogue above) |

---

## 5. How the game was built

The asset pipeline is visible in the shipped files:

- **BankCreator** — Lionhead's asset-bank compiler. It generated the headers in
  `data/Defs/RetailHeaders/` (`front_end_bank.h`, `gui_bank.h`, `text.h`, `fonts.h`,
  `particles.h`, `textures.h`), each stamped *"This file was auto-generated by
  BankCreator"*. Its exception classes (`CBankCreatorException`,
  `CBankCreatorAbortException`) are still linked into the retail executable, so the game
  contains bank-building code.
- **`.big` archives** — `text.big`, `fonts.big`, `dialogue.big` per language under
  `data/lang/<Language>/`, with `.lut` lookup tables beside them.
- **Definition compiler** — produces the `CompiledDefs/*.bin` set above from source
  definitions, with `names.bin` as the shared symbol table.

The practical consequence: UI is authored as **named definitions referencing banked
assets** (sprites by `EFrontEndGraphicBank` id, strings by `TEXT_*` id). New elements that
"match" should reuse those same banked assets rather than introducing new ones, because
adding to a bank means rebuilding it with a tool we do not have.

---

## 6. Runtime facts established by injection

Confirmed live, not inferred:

| Fact | Value |
| --- | --- |
| Component definition name | `*(char **)*(DWORD *)(component + 0x20)` |
| Render position (vertical) | `float` at `component + 0x038`; 30.0 per menu row |
| `CFrontEndList` children | `std::vector` triple at list `+0x164 / +0x168 / +0x16C` |
| Main menu | the `CFrontEndList` holding `UI_FRONTEND_BUTTON_QUIT` |

**The child vector is ownership, not the draw source.** Shrinking it still leaves the
dropped entry rendering; appending to it (even 226 times, from before construction) adds
nothing visible. Do not try to add elements that way.

**Text is re-read after the screen is on-screen**, so labels can be changed live — but only
if the search covers mapped and image memory, not just the heap. A heap-only sweep finds
roughly half the live copies and the visible label never changes.

**Position is live**; font size is not, and no scale field has been found in the component
(searched every float in a plausible range across `0x1000` bytes, plus every pointed-to
sub-object). Size appears to come from the definition at construction.

Two traps worth remembering:
- `VirtualProtect` to `PAGE_READWRITE` without restoring **strips EXECUTE**; the game dies
  on the next instruction fetch in that page.
- A memory scan whose needle is a string literal will rewrite its own needle inside the
  injected DLL. Assemble search patterns at runtime.

---

## 7. Adding a native UI element

The sanctioned path, in order:

1. **Pick a type** from the catalogue — `CText` (`0x06`) for a label, `CFrontEndButton`
   (`0x0B`) for a menu entry, `CComponentContainer` (`0x16`) for a panel, `CEditBox`
   (`0x25`) for text entry (an obvious fit for entering a host address).
2. **Allocate** its size with the game's allocator at `0x00BFEA1A`, which the mod already
   hooks, so the object lives in the same heap the game frees from.
3. **Construct** by calling the type's constructor with `this` in `ecx` and the single
   argument the factory passes.
4. **Attach** to a `CComponentContainer` / `CFrontEndScreen` via its own methods rather than
   writing the child vector directly — see the negative results above for why direct vector
   edits do not render.
5. **Position** via `+0x038`, which is verified live-editable.

Two ways to reach step 3 and 4 together, in increasing order of effort:

- **Definition swap** (already working, see `tools/re-probe`): rewrite the `push imm32`
  operand in `sub_59899A` before construction so the game builds a *different, larger*
  existing screen definition. Cheap, uses entirely native machinery, but limited to
  definitions that already exist.
- **Factory call / detour**: call `0x0041D21B` directly with a definition of our own, or
  detour it to append extra components while a screen is being constructed. This is the
  route to arbitrary new UI and the natural home for multiplayer panels. It needs a real
  trampoline detour, since the work has to happen *during* construction.

The open question for arbitrary elements is the definition object the factory consumes: we
know `def+0x3C` selects the type, but the rest of that structure is not yet mapped. The
next concrete step is dumping a live `CUIDef` (vtable `0x01259F8C`) for a known simple
component — a `CText` — and diffing it against a second one to map the layout fields.

---

## 8. `CUIDef` — the definition object (captured live)

Definitions are **`CUIDef` instances**, vtable `0x01259F8C`, confirmed by catching real ones
out of the factory. The type catalogue in section 3 is validated against live data: a
single frontend build produced definitions of types 0 (`CSprite`), 1 (`CMorphingSprite`),
5 (`CChangingStateComponent`), 6 (`CText`), 10 (`CFrontEndScreen`), 16 (`CTextSlider`),
22 (`CComponentContainer`) and 38 (`CNavButton`), each matching its decoded class.

### Layout so far

| offset | meaning |
| --- | --- |
| `+0x00` | vtable — `0x01259F8C` for `CUIDef` |
| `+0x04` | `1` on every definition seen — reference count |
| `+0x08` | **owning manager** — identical pointer across all definitions |
| `+0x0C`, `+0x10` | per-definition pointers (parent / sibling links) |
| `+0x14` | per-definition integer id (e.g. 994, 1747) |
| `+0x18` | small integer, varies |
| `+0x1C` | same pointer as `+0x08` |
| `+0x20` | second per-definition integer id (e.g. 621, 604) |
| **`+0x3C`** | **component type** — the factory's jump-table index |
| `+0x40` | pointer to a string object |
| `+0x44`, `+0x48` | equal to each other — an empty `std::vector` (`begin == end`) |
| `+0x60` | `1` on every definition seen |
| `+0x98`, `+0x9C` | font name on screen definitions — e.g. `"L_16"`; zero on `CText` |

`"L_16"` is a **font reference**, which is very likely where the oversized menu entry's size
comes from: fonts are banked (`data/lang/<lang>/fonts.big`, ids in
`data/Defs/RetailHeaders/fonts.h`), so an entry's text size is chosen by definition at
construction rather than scaled at draw time. That matches the earlier failure to find any
live scale field on the component.

### Reaching the definition manager

- `0x0044C6B0` is `mov eax,[0x013B879C]; ret` — a plain global read. **That global reads
  NULL from an injected thread**, so do not rely on it.
- `0x009AD390` is `GetDefinition(nameKey, index)`: it calls the name lookup at `0x009AD2E0`
  and then indexes `[result+8]` by `index`. It is *not* a name-only lookup.
- The reliable handle is **`*(DWORD *)(anyCUIDef + 0x08)`** — every definition points at the
  same manager instance. Grab one definition, and the manager comes with it.

### Technique: capturing definitions with INT3

Guard pages do not work under Wine (tried; the handler never fires). **`INT3` plus
`AddVectoredExceptionHandler` does.** Patch one byte at `0x0041D249` — the factory's
`mov eax,[ebx+0x3C]`, where `EBX` already holds the definition — and read `ContextRecord->Ebx`
in the handler, then restore the byte and set `Eip` back to re-run the real instruction.

Two things this needs to be right:

- **Never save `0xCC` as the original byte.** Re-arming while already armed will do exactly
  that, and the restore then writes `0xCC` back permanently.
- **Re-arm tightly.** A screen's children are constructed in a burst; each hit disarms the
  breakpoint, so re-arming on a 400 ms cadence caught 2 definitions where a `Sleep(1)` loop
  caught 24.

Keep the patch page `PAGE_EXECUTE_READWRITE` — dropping EXECUTE kills the game on the next
instruction fetch.

### What remains for authoring arbitrary UI

The definition is now readable and the manager reachable. The open items are the fields
between `+0x40` and `+0x98` that carry per-component layout (position, size, sprite id, text
id), which need diffing across two definitions of the *same* type — the capture run
collected only one `CFrontEndButton` definition, so that diff has not run yet. With those
mapped, a definition can be cloned, edited and handed to the factory at `0x0041D21B` to
produce a genuinely new, natively-styled component.
