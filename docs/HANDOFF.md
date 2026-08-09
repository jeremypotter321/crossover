# Handoff — Fable TLC modding

Everything needed to pick this up cold. Written 2026-08-09 at the end of a long session.

---

## 1. The three projects

| Repo | Path | Purpose |
| --- | --- | --- |
| **crossover** | `~/Documents/Projects/crossover` | Multiplayer mod (derived from EgoMP) + UI reverse engineering + SDK. Private, **not** a fork. |
| **fable-mod-loader** | `~/Documents/Projects/fable-mod-loader` | Mod loader with a plain-C ABI. Private. Works end to end. |
| *(retired)* | — | `jeremypotter321/crossover-fork-retired` — the original fork, still on GitHub, needs deleting (needs `delete_repo` scope). |

**Push is authorised for these repos only.** The global rule is still never-push everywhere else.

**The game:** Steam appid **204030**, installed into a Whisky bottle at
`~/Library/Containers/com.isaacmarovitz.Whisky/Bottles/3230151B-BEA6-48C5-8C7D-A7580586BF68/drive_c/Games/Fable`.
Launchers: `/Applications/Fable.app` and `/Applications/Fable Multiplayer.app`.

---

## 2. The development loop (this is the important part)

`crossover` itself needs MSVC (SLikeNet is a prebuilt MSVC C++ static lib) and **cannot be
built on the Mac**. But a standalone probe DLL links nothing, so it cross-compiles with
mingw-w64 and runs under Wine. That is the entire iteration loop:

```sh
cd ~/Documents/Projects/crossover/tools/re-probe
make            # builds probe.dll + inject.exe with i686-w64-mingw32-gcc
make run        # injects into Fable and launches
make log        # the probe's output
```

- `probe.c` — the current experiment (rewritten constantly; that is fine)
- `ui-probe.c`, `menu-entry.c`, `def-map.c` — preserved earlier experiments
- `Makefile` takes `SRC=` to pick which one to build
- Probe writes `probe.log` into the game directory

**Screenshots:** `scratchpad/shot-fable.sh <out.png>` captures the game window *by CGWindowID*,
so it works even when the window is behind others and never captures the user's screen.
It needs `scratchpad/shot-venv` (Python 3.13 + pyobjc-framework-Quartz).

**Static analysis:** `scratchpad/re-venv` (Python 3.8 + capstone + pefile) with
`re_tool.py` (xrefs, disasm) and `re2.py` (function boundaries, caller search).
`tools/re-probe/decode-factory.py` decodes the UI factory jump table.

---

## 3. Verified facts about Fable.exe

No ASLR, `ImageBase = 0x400000`, always. Every address below is absolute and live-verified.

### UI system (fully documented in `docs/ui-system.md`)

| Address | What |
| --- | --- |
| `0x0041D21B` | **UI component factory.** `thiscall(container, defId)` → component |
| `0x0041D249` | `mov eax,[ebx+0x3C]` — definition resolved (EBX), component not yet built |
| `0x0041D7F8` | factory jump table, 44 component types |
| `0x00BFEA1A` | game allocator (already hooked by `Core/SDK`) |
| `0x009AD410` | **name → definition id** |
| `0x013B86A0` | a *working* definition-manager global |
| `0x013B879C` | another manager global — **reads NULL from an injected thread, do not use** |
| `0x0099EBF0` / `0x0099EAE0` | `CharString` ctor / dtor |
| `0x00595356` | `CFrontEndManager` ctor (its code block runs to ~`0x59C000`) |
| `0x0059899A` | opens the main menu; picks the def by save state |

**`CUIDef` layout:** `+0x08` owning manager · `+0x14` record id · **`+0x20` def id (what the
factory takes)** · **`+0x3C` component type** · `+0x40..48` `vector<CUIStateDef>` (124 bytes
each, one per visual state) · `+0x58/+0x5C` floats · **`+0x70..78` `vector<uint32>` child
definition ids**.

**Components:** `+0x20` → CharString → definition name · **`+0x038` render Y (30 per menu
row, live-editable, proven)**. Horizontal position field is **unknown** — `+0x034` disproved.

### Guild seal (current task)

| Address / name | What |
| --- | --- |
| `OBJECT_GUILD_SEAL_1` | the seal definition (also `OBJECT_GUILDSEAL_BASE` / `_FULL`) |
| `CREATURE_HERO_CHILD` | the tutorial hero definition |
| `0x0058EEEC` | **function that resolves the seal by name** — contains the whole give sequence, takes the recipient at `[ebp+8]` |
| `0x0058EF87` | the `push "OBJECT_GUILD_SEAL_1"` inside it |
| `GiveHero` / `TakeObjectFromHero` | script commands; the dispatcher is a `strncmp` chain around `0x00CC6390`, implementations inline |

---

## 4. What works

- **Component construction** — calling the factory with a captured `(container, defId)` returns
  a real, correctly-typed component. Proven.
- **Attachment** — appending a definition id to a screen's `CUIDef+0x70` child list, before
  construction, makes the component render. Proven: injected text appeared on the main menu
  and on other screens.
- **Text rewriting** — sweeping memory for a string and overwriting it works, *if* it runs
  from t=0 (before glyphs are baked) and covers mapped/image memory, not just the heap.
  **"Hello World" has rendered in-engine in the game's font.**
- **The mod loader** — injects, discovers mods, version-checks and starts them.

## 5. What does not work yet

| Item | Status |
| --- | --- |
| "Hello World" on the main menu, right side | Text renders, but the *injected* definition and the *rewritten* string are different definitions. Pin one definition id and rewrite that one string. |
| Horizontal position | Field unidentified. Find it the way `+0x038` was found: write a distinctive value and observe. |
| Inline hook at `0x0041D249` | Installs, then kills the game even patching one screen. Disabled behind `ENABLE_INLINE_HOOK`. **Prime suspect: `VirtualAlloc` can place the stub >2GB away, overflowing the `jmp rel32`.** Fix: allocate near the image and assert the delta fits. |
| Guild seal | Next step is calling `0x0058EEEC` with the hero once one exists. |

---

## 6. Traps that cost real time — do not repeat

- **A duplicated component is invisible.** It inherits its twin's coordinates and draws exactly
  on top. Always offset it before concluding anything.
- **`CFrontEndList+0x164` is not the draw source.** It is an ownership list. Shrinking it does
  not remove a rendered entry; 226 appends changed nothing. Children come from `CUIDef+0x70`.
- **Guard pages never fire under Wine.** INT3 + `AddVectoredExceptionHandler` does.
- **Re-arming an INT3 while already armed** saves `0xCC` as the "original" byte and corrupts
  the restore. Save the original exactly once.
- **Re-arm tightly.** Components are built in a burst — `Sleep(1)` caught 24 definitions where
  a 400 ms cadence caught 2.
- **`VirtualProtect` to `PAGE_READWRITE` without restoring strips EXECUTE** and the game dies
  at the next instruction fetch in that page.
- **A memory scan whose needle is a string literal rewrites its own needle** inside the probe
  DLL. Assemble patterns at runtime.
- **Reinterpreting heap pointers as floats prints `0.000`** and hides every interesting field.
  Rule out strings and pointers before floats.
- **Rewriting short strings during startup crashes the game** — "Game Options" matches ~20
  places. Use long distinctive needles.
- **Do not trust a launcher's exit code for injection.** It returns 0 whether or not
  `LoadLibrary` succeeded in the target. Check the module is mapped:
  `lsof -p $(pgrep -f 'Fable\.exe') | grep -i <dll>`.
- **`__attribute__((naked))` on x86 GCC is unreliable.** Hand-emit trampoline bytes.
- **Never call Win32 from inside a hook handler** on the game's dispatch path. Pre-allocate.
- **Removing `data/Video` makes things worse** — the dialog becomes a guaranteed
  `0x80070003` file-not-found instead of an intermittent codec error. Leave it alone.

---

## 7. Environment gotchas

- **Wine video dialog (`0x80040218`)** intermittently blocks the menu. `wineserver -k`
  sometimes clears it. `osascript` cannot click it (no accessibility permission), and
  `PostMessage` of Return/Space does not dismiss it (the game reads DirectInput).
  `userst.ini` already has `SetPlayIntro(false)` — worth investigating why it still plays.
- **`gh` token lacks `workflow` and `delete_repo` scopes.** So the CI workflow cannot be
  pushed (it is parked on the local branch `ci/windows-build`) and the retired fork cannot be
  deleted. Fix: `gh auth refresh -h github.com -s workflow -s delete_repo` **run in a real
  terminal** — through this session it gets backgrounded and the device-code poll dies.
- The user is often not at the Mac. Prefer things that can be verified from the log rather
  than things needing someone to look at the screen.

---

## 8. Suggested next actions, in order

1. **Guild seal:** call `0x0058EEEC` with the hero as its argument once a hero exists, using
   the inline-asm thiscall helpers already in `probe.c`.
2. **Fix the inline hook** (`rel32` range), which removes all the INT3 re-arm racing.
3. **Finish "Hello World"**: pin one definition id, rewrite that definition's string, find the
   horizontal field.
4. Push the CI branch and delete the retired fork once the token scopes are refreshed.
