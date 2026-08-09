# Getting full control of Fable's UI

Written after a session that got partial control and hit the ceiling of the runtime-injection
approach. This records what that ceiling is and the two routes through it.

## What the ceiling is

Runtime injection can build and attach components, but only for screens built through the
component factory at `0x0041D21B`. Measured: a full load **plus** opening the Escape menu
produced 69 factory hits and exactly **one** screen definition (`rec=1739 def=215`). The
Escape menu's own definitions never appear. So `CUIDef+0x70` attachment cannot reach it.

Also learned the hard way, and worth not repeating:

- A definition **name** resolves to the **record id** (`CUIDef+0x14`), not the **def id**
  (`+0x20`) the factory is driven by. Comparing a name lookup against `+0x20` never matches.
- The in-game menu is constructed **once**, during the level load — not on each open. Any
  hook must be armed in `DllMain`, not after the definition manager appears.
- Calling into game code from the injected thread races the game thread. It killed a live
  session mid-frame. Reads are safe; calls are not, unless made on the game's own thread.

## Route A — the dev console Lionhead left in the retail build

The whole console is compiled in. Evidence, all from `Fable.exe`:

| Symbol / string | Where |
| --- | --- |
| `.?AVCInputProcessConsole@@` | RTTI |
| `.?AVCConsoleInputBase@@`, `.?AVCConsoleVariable@@` | RTTI |
| `.?AVCConsoleCommandBase@@`, `.?AVCConsoleClasslessCommand@@` | RTTI |
| `"Init Global Console"` | `0x004A7104` |
| `"Adding Console Commands"` | `0x004A7133` |
| `"Adding Console Variables"` | `0x0041863E` |
| `CommandList`, `CommandListContaining`, `VarList`, `VarListContaining` | `0x009ED4BD`, `0x009ED52F`, `0x009ED59E`, `0x009ED610` |
| `BindKey`, `BindString`, `RunBoundString`, `RunScript` | `0x009ED34A`, `0x009ED1F1`, `0x009ED28F`, `0x009ED3E6` |
| console colour/alpha commands | `0x009ED67F`..`0x009ED9D3` |

`CInputProcessConsole` is an input processor of exactly the kind already reversed:
`CInputProcessQuickAccessItems` has vtable `0x01237AE0`, is constructed at `0x00486860`, and
its base ctor `0x00687A30` copies four context pointers into `+0x14..+0x20`. The console
class will follow the same shape.

Why this is the high-leverage route: `CommandList` and `VarList` enumerate **every** command
and variable the engine registers — the game's own control surface, self-documenting, and
far broader than the UI. `BindKey` then binds any of it to a key.

Steps:

1. Find `CInputProcessConsole`'s vtable via its RTTI type descriptor → complete object
   locator → vtable (the same walk that identified `CInputProcessQuickAccessItems`).
2. Find its constructor (`mov [reg], <vtable>`) and who calls it — that shows whether the
   console is instantiated at all in retail, or compiled in but never constructed.
3. If constructed: find what suppresses it. `userst.ini` ships `AllowDebugProfile FALSE` and
   `ShowDevFrontEnd FALSE`; neither string is in the exe, so they are parsed elsewhere —
   find the variable they set.
4. If never constructed: construct one from an injected DLL and register it with whatever
   holds the input processors, using the captured context struct.
5. With a console: `CommandList` → the full vocabulary, then drive the UI from there.

## Route A — measured results

`tools/re-static/rtti.py` resolves class name -> vtable properly (it validates the complete
object locator; a naive "first dword pointing at the type descriptor" matches base-class
arrays and finds nothing for these classes). Verified against a known answer: it reproduces
`CInputProcessQuickAccessItems` -> `0x01237AE0`.

| Class | vtable | Constructed at |
| --- | --- | --- |
| `CConsole` | `0x0129C600` | `0x009ECD85`, `0x009ECF38` |
| `CTBaseSingleton<CConsole>` | `0x0129C44C` | — |
| `CInputProcessConsole` | `0x01237F24` | `0x0048B380` (ctor), installed at `0x00489C63` |
| `CConsoleCommandLine` | `0x0129C400` | eight sites in `0x009EA2C2`..`0x009EEB28` |

`CInputProcessConsole` is installed into **slot `+0x1F8`** of the input-processor manager,
by the same code that installs the other processors:

```asm
mov  ecx, dword ptr [esi+0x1F8]   ; existing
mov  dword ptr [esi+0x1F8], edi   ; <- the new CInputProcessConsole
call dword ptr [edx]              ; destroy the old
```

**Measured live, read-only, at the frontend:**

- `CConsole` **is instantiated** at `0x02C9DF68` — vtable correct, two embedded
  `CConsoleCommandLine` objects at `+0x1C` and `+0x2C`, and three more command lines live
  elsewhere. The console object genuinely exists in the retail build.
- `CInputProcessConsole` did **not** scan as live at the frontend. Neither did
  `CInputProcessQuickAccessItems`, which is known to be live in-game — so the processors are
  created on level load, not at startup. Re-scan in-game before concluding anything.

`CConsole` field map so far (from `0x02C9DF68`):

| Offset | Value | Reading |
| --- | --- | --- |
| `+0x04`, `+0x10` | `0x02C9E018`, `0x02C9E040` | containers, not yet walked |
| `+0x1C`, `+0x2C` | `0x0129C400` | embedded `CConsoleCommandLine` |
| `+0x40`/`+0x44` | ptr / `0x0F` | pointer+count pair |
| `+0x4C`/`+0x50`/`+0x54`/`+0x58` | ptrs / `9` | pointer+count pair |
| `+0x6C`, `+0x70`, `+0x7C` | `0x60`, `0x29`, `0xFF` | probably geometry / colour |

**Re-scanned in-game, with the probe's own stack region excluded — both questions answered:**

- `CInputProcessConsole` **is live in-game: 4 instances** (`0x057777D0`, `0x0836AF40`,
  `0x083811C0`, `0x08382720`), exactly mirroring the 4 live `CInputProcessQuickAccessItems`.
  They share context pointers `+0x14 = 0x05616408` and `+0x1C = 0x05737E70`, the ctx[0]/ctx[2]
  copied by the base ctor `0x00687A30`. The console's input processor is constructed and
  installed in the retail build. Nothing needs to be created — only activated.
- `CConsole+0x04` and `+0x10` are **empty lists** — `[+0x08] == [+0x0C] == self`, the MSVC
  empty-list sentinel. Not the registry.
- `CConsole+0x40/+0x44` **is** a live container, and its count **grew 15 → 18** between the
  frontend and in-game, so it accumulates at run time. `+0x40` derefs to three real pointers
  (`0x0835B868`, `0x080F5740`, `0x055E6590`). This is the one to walk.
- `CConsoleCommandLine` instances carry inline command text: `0x02CA1CE8` holds fragments
  `"etR"`, `"utio"`, `"n"` — i.e. a small-string buffer for something like `SetResolution`.
  So command strings are reachable from these objects.

### The manager, and the activation difference

Found by scanning for whatever points at a live console processor: the input-processor
manager is at `0x0837FA08` (and the console processor's own `+0x18` points back at it, so
`ctx[1]` is the manager). It holds a dense array of processor pointers:

```
+0x160 = quick-access  0x05776CE8      <- active
+0x1E0 .. +0x204         ten more processors
+0x1F8 = console       0x057777D0      <- inactive
+0x208 = 00000001
```

There are four managers in play (`0x0837FA08`, `0x08369A70`, `0x0837FD40`, `0x083812C0`),
which is why every processor class scans as four instances -- one per manager.

**The difference between the active and inactive processor is in the object, not the slot:**

```
quick-access 0x05776CE8:  +04=05612CF0  +08=05776CE8  +0C=05776D28  +10=05776CB0
console      0x057777D0:  +04=00000000  +08=00000000  +0C=00000000  +10=00000000
```

Both are registered in the manager's array; only the quick-access one has `+0x04..+0x10`
populated. Note `+0x08` points at *itself* -- an intrusive list node. The base ctor
`0x00687A30` only writes `+0x14..+0x24`, so `+0x04..+0x10` come from the class below it
(`0x00A0D290`, called first). That is almost certainly enrolment in the active input chain:
the console processor is *installed* but never *enrolled*.

### `CAInputProcess`, confirmed

`0x00A0D290` is the base ctor and it does exactly one interesting thing: it sets vtable
`0x0129CA10` and **zeroes `+0x04`, `+0x08`, `+0x0C`, `+0x10`**. RTTI says that vtable is
`.?AVCAInputProcess@@` -- the base of every input processor. So those four fields start empty
on *every* processor and are filled later by whatever engages one.

Its first vtable slots (`0x00486360`, `0x00486380`, `0x00486390`) are empty base
implementations (`ret` / `ret 4`), so the behaviour is all in the overrides.

Worth noting before chasing "enrolment" too literally: only **one** quick-access instance has
`+0x04..+0x10` populated -- the one belonging to manager `0x0837FA08`. The other three
quick-access instances are zeroed exactly like all four console ones. So the fields may mark
*the currently engaged processor of the active manager* rather than a permanent registration.
Both readings lead to the same next move.

Those were the next steps while activation still looked like the way in. It is not — see
below. Activating the input processor was never needed, because the console already runs
scripts, and the vocabulary it would have exposed turns out not to reach the UI.

## Route A — CLOSED. The console is already open, and it cannot drive the UI

Two findings, one static and one measured, close this route.

### The console's input surface is `user.ini` / `userst.ini`

`0x009EC890` is `CConsole::RunScript(filename)`, and the game hands it both ini files
during startup:

| Address | What |
| --- | --- |
| `0x013CAA40` | **the `CConsole*` singleton** (ctor `0x009ECD80`, installed by `0x00413520`) |
| `0x00414C90` | `GetConsole()` — returns the singleton, constructing it on first use |
| `0x009EC890` | `CConsole::RunScript(CharString *filename)` |
| `0x00414C7F` | runs **`userst.ini`**, gated on the byte at `0x01375444` |
| `0x00418981` | runs **`user.ini`** |
| `0x009EC5E0` | `CConsole::AddCommand(CConsoleInputBase*)` |

So every line of those files is a console statement. That is why `user.ini` contains
`RunScript("joystick.ini");` and `ActivateQuest("Gameflow");` — both are real registered
commands. **No activation, no injection and no input processor is required to issue a
console command: put it in the ini.** Slot `+0x1F8` only ever mattered for typing at a
prompt.

It also explains a long-standing puzzle from `HANDOFF.md` §7. `SetPlayIntro(false)` is in
`userst.ini` and does nothing because **`SetPlayIntro` does not exist in the retail exe** —
neither do `SetResolution`, `ShowDevFrontEnd`, `AllowDebugProfile`, `SetSkipFrontend`,
`UseLevelWAD`, `PresentImmediate`, `SetDefinitionValidation`, `SetRunScripts`,
`SetMaxAnisotropy`, `MaxThingDrawDist` or any of the rest. Those names appear nowhere in
`Fable.exe` as ASCII or UTF-16. The ini files are Lionhead's dev-era scripts shipped intact;
retail kept the interpreter and compiled out most of the vocabulary, and unknown statements
are ignored silently. Chasing `ShowDevFrontEnd FALSE` any further is chasing a name with no
code behind it.

### The whole vocabulary, and why it is not enough

`tools/re-static/console-cmds.py` recovers the table straight out of `.text`. Every entry is
built by the same idiom — allocate, construct a `CharString` name, store the vtable, store
the payload, `AddCommand` — so name, class and payload are all immediates:

| Class | vtable | size | layout |
| --- | --- | --- | --- |
| `CConsoleClasslessCommand` | `0x0122E65C` | `0x18` | `+0x04` name · `+0x14` handler |
| `CConsoleCommand<T>` | `0x0129C4E0`, `0x0125D668` | `0x1C` | `+0x04` name · `+0x14` owner · `+0x18` handler |
| `CConsoleVariable` | `0x0122E5C8` | `0x10` | `+0x04` name · `+0x08` type · `+0x0C` **storage address** |

That is **20 commands and 22 variables**. The commands are `ActivateQuest`, `SetLevel`,
`SetStartingHolySite`, `EnableCodeSectionLoading`, `SetTimeOfDay`, `SetDaySpeed`,
`BindKey`, `BindString`, `RunBoundString`, `RunScript`, `CommandList`, `VarList`, the two
`*Containing` search commands, `ConsoleListContaining` and five console-colour setters. The
variables are all boot-time data-pipeline switches (`UseCompiledDefs`, `RunFromDVD`,
`AllowDataGeneration`, the pool sizes).

**Not one of them touches the UI.** The premise this route was run on — that `CommandList`
would hand over most of what is wanted — is measurably false. `CommandList` was worth
running; it just does not say what was hoped.

### Measured live, read-only (`tools/re-probe/console-vocab.c`)

- The singleton at `0x013CAA40` holds `0x02C9DF68` with the correct vtable, confirming the
  global. No scanning needed any more.
- **All nine variables `userst.ini` sets match their live bytes**, and the `userst.ini`
  gate at `0x01375444` is `1`. The ini really is executed.
- `CConsole+0x44` counts **18**, and a vtable scan finds exactly those 18 — every name and
  every handler address identical to the static table. The static recovery is complete.
- The 18 are the 14 `Command<CConsole>` + 2 `Command<CGameTimeManager>` + `ActivateQuest` +
  one variable. So the boot-config entries (`SetLevel`, `SetStartingHolySite`,
  `EnableCodeSectionLoading` and all 22 variables) are registered, executed from the ini,
  and then **freed** — one-shot boot configuration.
- One entry exists only at run time: **`ConsoleAlpha`**, a variable whose storage is
  `CConsole+0x7C` (the `0xFF` seen in the earlier field map). Registered by code, not by the
  static idiom — the only thing the static pass could not have found.
- Probing at **t=4s during startup**, before the boot entries are freed, catches the
  registry at its peak: **40 live entries, every name and payload address identical to the
  static table** — including all 22 variables' storage addresses and the three boot-only
  commands. The two absent are the `CGameTimeManager` pair, registered later. The static
  recovery is complete in both directions.
- `CConsoleVariable+0x08` is a type code, readable from those live objects: **`1` = int,
  `3` = float, `4` = bool byte**. It is stored from a register, so the static scan cannot
  see it; `LandscapePhysicalMemoryRatio` reading `0xBF800000` (= `-1.0f`) with type `3`
  confirms the mapping.

Everything above `Route A — CLOSED` stands as measured; it is simply no longer the way in.

## Route B — the compiled definition files

`data/CompiledDefs/{game,frontend,script}.bin` are the actual source of truth for every UI
screen: children, layout, states, text. `ui-system.md` calls reversing this container a dead
end — that was a **cost** judgement made while runtime injection still looked cheaper, not a
statement that it is impossible.

If the goal is total control, this is the only route that actually delivers it: every screen
editable as data, persistent across runs, no injection, no thread races, no crashes, and it
reaches screens the factory never touches — including the Escape menu.

What is already known: the shared header (leading zero byte, 4-byte checksum, magic
`0xA8E36C34`, definition count), the counts (`game.bin` 14,761, `frontend.bin` 810,
`script.bin` 611), and `names.bin`'s format in full (hash + name, 13,593 entries). The live
`CUIDef` layout is mapped, which gives a target structure to match records against.

The lever that makes this tractable: dump a definition **live** from memory (already
possible), then find that same record in the file by its distinctive field values. That
converts format reversing from cold analysis into a matching exercise.

### The def *writer* is still in the exe — but it is dead code. TESTED.

This came out of Route A. The static case looked strong; it does not survive contact.

The definition loader branches on the runtime copy of `UseCompiledDefs` at `0x009B08E2`:

```asm
mov  al, byte ptr [0x13CA7D8]     ; UseCompiledDefs, runtime copy
test al, al
je   0x009B09BB                   ; FALSE -> the uncompiled path
...                               ; TRUE  -> open(file, mode 1)  = READ  @0x009B091F
0x009B09BB:                       ; FALSE -> ... open(file, mode 4) = WRITE @0x009B0A26
```

Both branches operate on the same file object at `defmgr+0xB8`; only the mode differs.
The write side is gated on the byte at `0x0138E188`, and that byte is a straight copy of
the `AllowDataGeneration` console variable:

```asm
0x004025D5  mov al,  byte ptr [0x1375459]   ; AllowDataGeneration
0x004025DA  mov byte ptr [0x138E188], al    ; data-write flag
0x004025DF  mov byte ptr [0x138E189], al
0x004025E4  mov byte ptr [0x138DD3B], al
```

`UseCompiledDefs` (`0x013B8617`) and `AllowDataGeneration` (`0x01375459`) are both live,
registered console variables that `userst.ini` already sets — currently `TRUE` and `FALSE`.
Flipping them is a two-line text edit, no injection at all.

If the write path did what its shape says, the retail build could serialise the compiled def
format itself — far better than reversing the reader.

**Run 2026-08-09. It does not. `AllowDataGeneration TRUE` crashes retail at startup.**

| Run | `UseCompiledDefs` | `AllowDataGeneration` | Result |
| --- | --- | --- | --- |
| 1 | `FALSE` | `TRUE` | page fault reading `0x00000010` at **`0x009FC036`** |
| 2 | `TRUE` | `TRUE` | page fault reading `0x00000010` at **`0x009FC036`** — same address |
| 3 | `TRUE` | `FALSE` (stock) | boots normally |

The flags did take effect — a probe injected at launch read `0x0138E188`, `0x0138E189` and
`0x0138DD3B` all `1`, and `0x013CA7D8` (runtime `UseCompiledDefs`) `0` in run 1 and `1` in
run 2. So this is not a misapplied setting; the code behind the flag genuinely faults.

**Run 2 is the one that matters.** It kept `UseCompiledDefs TRUE`, so the def loader took
its normal compiled-read path, and it still died at the identical address. The crash is
caused by `AllowDataGeneration`, not by the uncompiled def path — which also means run 1
never got far enough to say anything about the def branch at all.

`0x009FC020` is a small intrusive-list unlink helper — zero `[node+8]`, splice out via
`+0x0C`/`+0x10`, decrement a count at `[owner+0x20]` and subtract a size from `[owner+0x24]`:

```asm
0x009FC020  mov  eax, [esp+4]         ; the node
0x009FC024  mov  edx, [eax+0x10]      ; next
0x009FC02A  mov  dword ptr [eax+8], 0
0x009FC031  je   0x009FC05A           ; next == NULL is handled
0x009FC033  mov  esi, [eax+0x0C]      ; prev -- NOT checked
0x009FC036  cmp  dword ptr [esi+0x10], eax   ; <- faults, prev == NULL
```

It guards `next` and not `prev`, so it faults the first time data generation asks it to
evict from an empty pool. Dev-only bookkeeping that no retail run ever exercises.

**Nothing was written.** All 393 files under `data/` hash identically before and after all
three launches; `CompiledDefs/*.bin` kept their original mtimes. `userst.ini` was restored
byte-for-byte and the game is verified booting normally again.

So the two-line shortcut is closed. It could in principle be forced — the fault is a single
unguarded `prev`, and patching `0x009FC033` to skip when `[eax+0x0C]` is NULL would get
past it — but that is patching retail code to drive a dev path with unknown further
dependencies, and it is speculation until someone tries it. Not recommended ahead of the
matching plan below.

## Recommendation

**Route A is closed** — done, not abandoned. The console is already open (the ini files are
its script input) and its entire 42-entry vocabulary reaches levels, quests, time, key
bindings and the boot data pipeline, but never the UI. Do not spend more time on activating
the input processor; it buys a prompt for commands that do not exist.

**Route B is now the only route, and the writer shortcut has been tried and failed.** The
engine's own def serialiser is unreachable — `AllowDataGeneration TRUE` faults at startup in
retail (measured, twice, above). So Route B is the live-dump-and-match plan: dump a
definition from memory, find that record in `game.bin` by its distinctive field values, and
grow the format outwards from the matches. That is still the tractable version of cold
analysis, and it is now the only version.

Do not extend the runtime-attachment work further. Its reach has been measured and the
Escape menu is outside it.
