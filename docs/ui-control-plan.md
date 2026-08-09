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

## Recommendation

Run A first — it is days of work, not weeks, and `CommandList` may hand over most of what is
wanted without any further reverse engineering. Then B as the durable answer, because it is
the only one that gives control over screens the factory never constructs.

Do not extend the runtime-attachment work further. Its reach has been measured and the
Escape menu is outside it.
