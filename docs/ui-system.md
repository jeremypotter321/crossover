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

---

## 9. `CUIStateDef` — where the layout actually lives

The scalar diff of two same-type definitions turns up very little, because most per-component
content is not in `CUIDef` at all. `CUIDef+0x40` is a **`std::vector<CUIStateDef>`**:

```
CUIDef +0x40   begin
       +0x44   end
       +0x48   capacity          (== end: these vectors are exactly sized)
       sizeof(CUIStateDef) = 0x7C (124)
```

`CUIStateDef` is confirmed by RTTI — the records begin with vtable `0x0125871C`, whose
complete-object locator resolves to `.?AVCUIStateDef@NUISystem@@`.

**A component has one record per visual state.** Observed live: `CSprite` 1 state, `CText`
2 and 4, `CFrontEndScreen` 8. That is what makes a widget change appearance on hover, press
and disable — and it means a component's *appearance* is per-state data, not a single value.

### `CUIDef` scalar fields identified

| offset | meaning |
| --- | --- |
| `+0x14`, `+0x18`, `+0x20` | per-definition integer ids |
| `+0x40` … `+0x48` | `std::vector<CUIStateDef>` |
| `+0x58` | float — position, e.g. `30.00f` on a sprite |
| `+0x5C` | float — position, e.g. `250.00f` on a sprite |

### `CUIStateDef` fields identified

From diffing state `[0]` across definitions of the same type:

| offset | observed | reading |
| --- | --- | --- |
| `+0x00` | vtable `0x0125871C` | `CUIStateDef` |
| `+0x24` | `-256`, `-160`, `-144`, `-80`, `-8` | signed offset — negative multiples of 8/16 |
| `+0x3C` | `273`, `50` | asset id — the banked graphic (`EFrontEndGraphicBank`) |
| `+0x40` | `34`, `7` | second id |
| `+0x48` | `256.0f`, `-140.0f`, `-80.0f`, `65.0f`, `60.0f` | coordinate / extent |
| `+0x4C` | `44.0f`, `0` | coordinate / extent |
| `+0x50` … `+0x64` | mostly `1.0f`, one `-1.0f` | colour and scale factors |
| `+0x28` | `255` | alpha |

The `1.0f` run at `+0x50`–`+0x64` with a trailing `-1.0f` is the shape of a colour/scale
block left at defaults, which is consistent with an authored definition that only overrides
what it needs.

Semantics of the individual state fields are **inferred from value shape**, not yet proven
by writing to them. The clean way to nail each one down is to write a distinctive value into
a live state record and watch which visual property moves — the same method that proved
`component+0x038` is the render position.

### What this unlocks

The pieces for authoring a native component are now all identified: pick a type from the
catalogue, build a `CUIDef` with that type at `+0x3C`, give it a `std::vector<CUIStateDef>`
with at least one 124-byte state carrying the banked asset id and coordinates, and hand it to
the factory at `0x0041D21B`. Cloning an existing definition of the desired type and editing
those fields is far safer than constructing one from nothing, because it inherits every field
that is still unmapped.

---

## 10. Building a component — proven

The construction loop works. Verified live by capturing a real factory invocation and then
re-issuing it from an injected thread:

```
captured this=0x055E5780  arg1=0x0000025C      (604)
factory returned component 0x086AED20
component vtable = 0x012497E4                  -> CFrontEndScreen
component def    = UI_FRONTEND_MEDIA_PLAYER_ERROR
```

**The factory does not take a definition pointer.** It takes a definition **id** and resolves
the definition itself through the manager (`0x0042AEDA`). That id is the value at
**`CUIDef+0x20`** — the captured `arg1` of 604 is exactly the `+0x20` field of the screen
definition captured earlier.

So the call is:

```c
void *component = CUIFactory::CreateComponent(container, def->GetDefId());
```

where `container` is an object holding a definition manager at `+0x64` — the screen or list
the component is being built for. Both come free inside a `CUIFactory` hook, which is the
natural place to add components to a screen while it is under construction.

This also explains why `CUIDef` carries two integer ids: `+0x14` identifies the definition
record, while `+0x20` is the handle the construction path is driven by.

### The remaining unknown

Construction is solved; **attachment is not**. A component built this way is a valid, live
object, but nothing has yet placed it into a screen's draw set — and the draw set is fixed at
construction, as section 6 records. The next step is to hook the factory, let the game build
a screen, and observe what the caller does with each returned component: whatever call
follows the factory in that caller is the attach path.

---

## 11. Attachment — partially mapped, not yet functional

### The create path is now complete

```
0x009AD410   name -> definition id          (returns the id in eax)
0x0041D21B   factory(container, defId)      -> component
             component->vtable[0x14C](arg)  -> initialise
```

The wrapper around `0x0041DB49` does exactly this: resolve an id, call the factory, then
dispatch the component's own virtual at **`+0x14C`** to initialise it before returning. Any
component we build should be initialised the same way.

### One attach mechanism found, but it is not the general one

`0x00535AD0` is a `push_back` of **8-byte refcounted pairs** `{ component*, int* refcount }`
into a `{begin,end,capacity}` vector, paired with `0x00429C15` (construct the pair,
allocating a 12-byte refcount block) and `0x004291DE` (release it). It only writes when
`end != capacity`.

```
mov  [eax], esi            ; pair.ptr
mov  [eax+4], edx          ; pair.refcount
inc  dword ptr [edx]       ; refcount++
add  dword ptr [ecx+4], 8  ; end += 8
```

**But it is not how menu components are attached.** Two independent checks:

- Scanning memory for an 8-byte-stride run of the main menu's own buttons found **nothing**.
- An INT3 on `0x00535AD0` across a whole frontend build captured **zero** calls.

So this belongs to one specific caller (the `CList` path at `0x0053B871`), not the general
component tree. Do not build on it without checking it fires for the container in question.

### Next step

Attachment happens in the **callers of the create-and-initialise wrapper**, not in the
factory or the wrapper itself. The way to find it is the method that has worked throughout:
INT3 the wrapper, capture its return address, and disassemble the caller immediately after
the call — whatever it does with the returned component is the attach path. Guessing at
container layouts has produced only false leads.

---

## 12. ATTACHMENT FOUND — `CUIDef+0x70` is the child list

A screen does not have components pushed into it. **It builds its own children, at
construction, from a list of definition ids in its own definition.**

```
CUIDef +0x70   begin      std::vector<uint32>  child definition ids
       +0x74   end
       +0x78   capacity
```

Dumped from a live `CFrontEndScreen` definition (type `0x0A`):

```
5 elements of 4 bytes:  605, 685, 120, 578, 606
```

Those are **definition ids** — the same values the factory takes as `arg1` (the captured
`604` earlier). So a screen's content is authored as a list of child definitions, and the
build is: for each id, resolve the definition, run it through the factory, initialise via
`vtable[0x14C]`.

### Why every earlier attempt failed

This single fact explains all of it, and each failure was actually consistent evidence:

- The drawn set is fixed at construction **because it is authored in the definition**.
- Appending to `CFrontEndList+0x164` did nothing — that vector is downstream bookkeeping,
  not the source of truth.
- Shrinking that vector did not remove a rendered entry, for the same reason.
- Swapping the *whole definition* to `UI_FRONTEND_MAIN_MENU` DID add an entry — because a
  different definition carries a different child-id list. That worked by accident of being
  the only thing that touched the real mechanism.

### How to attach a component

Append a definition id to the screen's child list **before the screen is constructed**. The
window is exactly the factory breakpoint at `0x0041D249`: the definition has been resolved
but the component has not been built yet.

Because the vector is exactly sized (`end == capacity`), relocate it:

```c
/* at 0x0041D249, with `def` = EBX and def type == 0x0A */
DWORD b = *(DWORD *)(def + 0x70), e = *(DWORD *)(def + 0x74);
DWORD n = (e - b) / 4;
DWORD *nv = VirtualAlloc(NULL, (n + 2) * 4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
memcpy(nv, (void *)b, n * 4);
nv[n] = my_child_def_id;
*(DWORD *)(def + 0x70) = (DWORD)nv;
*(DWORD *)(def + 0x74) = (DWORD)(nv + n + 1);
*(DWORD *)(def + 0x78) = (DWORD)(nv + n + 1);
```

Implemented in `tools/re-probe`, which extended 7–10 screen child lists per run with the
game staying up.

> **Not yet visually confirmed.** The runs that carried this change were blocked by Wine's
> intermittent `0x80040218` video-playback dialog, which sits in front of the menu, so the
> resulting screen was never photographed. The mechanism is evidenced by the data (child ids
> in the definition, matching factory arg1 values, and the definition-swap result), not by a
> screenshot. Clear the dialog with `wineserver -k` and re-run to confirm visually.

---

## 13. CONFIRMED — a seventh native entry in the main menu

Done and seen on screen, 2026-08-24, by `tools/re-probe/menu-append.c`. The main menu
rendered **seven** rows, the seventh being a second `Quit`, and clicking it opened the
game's own "Are you sure you want to quit?" dialog. So the extra row is not a drawn
decoration: it is a real `CFrontEndButton`, constructed by the factory and wired to its
action by the game, exactly like the six it ships with.

### The correction that made it work: the buttons hang off the *list*, not the screen

Section 12 had the mechanism right and the target wrong. Appending to the **screen**
definition (type `0x0A`) adds a child to the screen, which is not where the rows live.
The menu rows are children of a **list** definition (type `0x0C`), and that is what has
to be extended.

Captured live, and the reason the six rows are the six rows:

| definition | type | children |
| --- | --- | --- |
| `215` main menu screen | `0x0A` | `245`, `200`, `685` |
| `245` main menu list | `0x0C` | `262`, `247`, `266`, `280`, `255`, `251` |

Those six ids are, in rendered order: Continue Game, Change Profile, Options, Credits,
About, **Quit = 251**. Appending `251` a second time is what produced the second Quit.

### The other correction: emulate the instruction, do not re-arm it

The INT3 at `0x0041D249` cannot use the usual restore-the-byte-and-re-run shape. Components
are built in a burst of hundreds of factory calls inside a few milliseconds, so between the
handler restoring the byte and a worker thread re-arming it, almost the whole burst goes
past unseen. The first run of this probe caught four definitions out of dozens and the main
menu was not among them — which reads exactly like "the mechanism does not work".

There is no need to re-run the instruction. It is `mov eax,[ebx+0x3C]`: three bytes, no
relative operand, and `mov` writes no flags. Perform it in the handler and resume past it:

```c
ep->ContextRecord->Eax = *(DWORD *)(ebx + 0x3C);
ep->ContextRecord->Eip = 0x0041D249 + 3;
```

The INT3 then never leaves the code, every factory call is seen, and there is no race.

### Do not call the definition manager from your own thread

Reaching the manager works — it is at `container+0x64`, captured from a factory call. But
calling `GetDefinition` (`0x009AD390`) on it from the probe's thread killed the process
every time, while the frontend was being built. Identify definitions by their child ids,
which is a pure read, rather than by name.

### What is still open

The seventh row is a *stock* button, because a duplicated stock id is all that can be
appended so far: an id we invent does not resolve through the manager. Giving the row our
own label and our own action needs either a definition the manager will resolve, or a
post-construction rewrite of the built component's text and handler.

---

## 14. A menu row's label — found, and the reader bug that hid it

### The chain

A row's text is not in the button. It is three objects down:

```
button            type 11  CFrontEndButton            6 states
 └─ middle        type 5   CChangingStateComponent    5 states, 2 children
     ├─ label     type 6   CText                      <- the text
     └─ art       type 2 -> type 0 CSprite (id 127, shared by all six rows)
```

and the CText definition names its string by **key**:

| offset | holds |
| --- | --- |
| `CUIDef +0x54` | pointer to a `CharString` of the **wide** text key |
| `CUIStateDef +0x20` | the same key, repeated in each of the 5 states |

Read live from the six main-menu rows:

| row | button | middle | CText | key |
| --- | --- | --- | --- | --- |
| Continue Game | 262 | 263 | 265 | `TEXT_GUI_MENU_CONTINUE_GAME` |
| Change Profile | 247 | 248 | 250 | `TEXT_GUI_MENU_CHANGE_PROFILE` |
| Options | 266 | 267 | 269 | `TEXT_GUI_MENU_OPTIONS` |
| Credits | 280 | 283 | 285 | `TEXT_GUI_MENU_CREDITS` |
| About | 255 | 256 | 257 | `TEXT_GUI_MENU_ABOUT` |
| Quit | 251 | 252 | 254 | `TEXT_GUI_MENU_QUIT` |

The resolved text (`Quit`, `Credits`, …) exists separately in the same heap, also
UTF-16 — so there are two strings per row, the key and the localised result.

### The bug that cost the most time here

Section 8 and every probe before this one reported "no string is reachable from any of
these definitions". **That was the reader, not the game.** The scanner walked bytes and
stopped at the first NUL, so a wide key (`'T', 0, 'E', 0, …`) measured one character long
and was discarded as noise. The game is localised; its text was never going to be ASCII.

The cost of that was two false trails, both pursued to an experiment:

- `CFrontEndButton CUIDef +0xC4/+0xE4` — a mirrored per-row integer (66, 16, 297, 67,
  321, 314). Writing Options' value over Quit's changed nothing.
- `CText CUIDef +0x38` — six distinct values that all fit in 16 bits, which looked exactly
  like a string-table index. It is **not stable across runs** (Quit was 36145 in one
  process and 1329 in the next), so it cannot be an authored id at all. Comparing the same
  field across two launches would have killed this in a minute, and is worth doing to any
  candidate id before building an experiment on it.

### Two techniques worth keeping

**Scan for definitions instead of catching them.** Every `CUIDef` starts with vtable
`0x01259F8C` and carries its id at `+0x20`, so one pass over committed memory finds any
definition by id — no breakpoint, no construction burst to race, nothing called in the
game. This is how the six label definitions were finally read, after the factory
breakpoint kept missing them: a child is constructed *after* its parent, and the probe
disarmed as soon as it had all six parents.

**Attach, do not relaunch.** `tools/re-probe/attach.exe` injects into the running game, so
an iteration costs nothing. Every relaunch replays the intro and re-rolls Wine's
`0x80040218` video dialog, which blocks the frontend from ever being built — and while it
is up, a probe waiting on the main menu waits forever.

### What is still open

Changing the label is not yet demonstrated. Two things were tried and neither is the
answer:

- Editing the definition after the menu exists does nothing — the text is baked into the
  component at construction, so a definition edit needs the screen rebuilt to show.
- Rewriting every occurrence of the resolved wide text `Quit` -> `Mods` in writable memory
  works when attached to a running game (16 sites rewritten, game stable), but doing the
  same continuously from process start **killed the game**. It is too blunt: most of those
  matches are in a system module's own string table, not the game's text bank.

The clean route is the key, not the resolved text: point a row's `CText +0x54` at a
`CharString` of our own before construction, and hook the key lookup so our key resolves
to our label. That keeps the change to one row and needs no memory-wide rewriting.

---

## 15. CONFIRMED — a row's label is chosen at construction

Seen on screen 2026-08-24 by `tools/re-probe/menu-swap.c`: the main menu's Quit row was
drawn with **Options'** text, because one id in one child vector was changed before the
component existed.

```
definition 252 (the CChangingStateComponent under Quit)
   children  [254, 73]  ->  [269, 73]
                 |               `- 269 is the CText Options' row uses
                 `- 254 is the CText Quit's row uses
```

Applied in the factory window at `0x0041D249`, the same one-instruction window that adds a
row. So the two halves of "put our own entry in the real menu" are now both proved:
**which rows exist** (section 13) and **what a row says** (this).

### Why the label had to be changed this way

There is no hot reload. A CText's string is baked into the component at construction, so a
definition edit made after the menu exists shows nothing until the screen is rebuilt — that
is why `+0x54` looked inert when edited from an attached probe.

Editing the *resolved* text instead does not work in that window either, and the reason is
worth recording: **the localised strings do not exist yet.** A probe scanning private
memory from process start never finds the wide `Quit` at all until the frontend loads,
which is the moment we are trying to get in front of. Definitions are loaded long before,
so the definition graph is the only thing available early enough to edit.

(And when that same resolved-text rewrite is done across *all* writable memory rather than
private memory only, it kills the game: 14 of its 15 matches were inside a loaded module's
own string table, not the game's text bank. Filter on `MEMORY_BASIC_INFORMATION.Type ==
MEM_PRIVATE`.)

### What remains for a label of our own

Swapping to another stock CText gives another stock label. An arbitrary one needs the key
at `CText +0x54` to resolve to text we choose — either by hooking the key lookup, or by
learning what the game does with a key that is not in the bank.
