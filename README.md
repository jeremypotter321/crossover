# crossover

Multiplayer for **Fable: The Lost Chapters**, plus a working setup for playing it on
macOS through Wine.

---

## Credit and origin

This project is a derivative of **[EgoMP](https://github.com/98thrxse/egomp)** by
**[98thrxse](https://github.com/98thrxse)**, released under GPL-3.0.

Effectively all of the hard work here is theirs: reverse engineering Fable's internal
class layout, building the SDK that wraps it, the DLL injection launcher, and the
SLikeNet networking and player-synchronisation layer. This repository exists to build on
that work, not to claim it. The upstream commit history is preserved in this repo, so
authorship of the original code remains visible in `git log`.

If you find this useful, go star [the original project](https://github.com/98thrxse/egomp).

---

## Status

Experimental. Expect crashes, desyncs, and half-finished features. This is something to
poke at, not something to rely on.

A legitimate copy of the game is required. No game assets are distributed here.

---

## How it works

Two artifacts, both 32-bit to match `Fable.exe`:

| Artifact | Role |
| --- | --- |
| `EgoMP.exe` | Launcher. Starts the game suspended, injects the core DLL, resumes it, exits. |
| `EgoMP.dll` | Core. Hooks the game with MinHook and runs the networking layer over SLikeNet. |

Injection is the standard `VirtualAllocEx` → `WriteProcessMemory` → `CreateRemoteThread`
on `LoadLibraryW` sequence. The hooks target **absolute addresses** in `Fable.exe` rather
than module-relative offsets, which works because the executable is a 2005 binary with no
ASLR — it always loads at its preferred base of `0x400000`. This also means the hooks are
tied to one specific build of the game.

### Layout matters

`EgoMP.exe` resolves the game as a relative `..\Fable.exe` and the core as `EgoMP.dll`
next to itself. It must therefore live in an `EgoMP/` subfolder of the game directory and
run with that folder as its working directory:

```
Fable/
├── Fable.exe
└── EgoMP/
    ├── EgoMP.exe
    └── EgoMP.dll
```

### In game

Get into the game world first — nothing responds at the main menu.

| Key | Action |
| --- | --- |
| <kbd>NUMPAD 1</kbd> | Host a session |
| <kbd>NUMPAD 2</kbd> | Connect to a session |
| <kbd>NUMPAD 3</kbd> | Disconnect |

Hosting prompts for a port; connecting prompts for a host IP and port. Empty input uses
defaults. Those prompts appear on the **console**, see the note below.

---

## Running on macOS

The mod runs under Wine. Verified on Apple Silicon (macOS 15.7) using
[Whisky](https://github.com/Whisky-App/Whisky), including the cross-process injection,
which Wine implements correctly.

**The Wine build needs 32-bit support.** `Fable.exe` is a 32-bit i386 binary, so the Wine
build must provide 32-on-64 thunking (Whisky's Wine ships `x86_32on64-unix`; a stock
64-bit-only Wine will not do). 

**Two things the game itself needs in the prefix:**

1. Registry keys under `HKLM\SOFTWARE\Microsoft\Microsoft Games\Fable\1.0` — `SetupPath`
   (REG_SZ, the install directory) and `LangID` (REG_DWORD, `0x409` for English). Because
   the game is 32-bit it reads these through WOW64 redirection, so write them to
   **both** `HKLM\SOFTWARE\...` and `HKLM\SOFTWARE\WOW6432Node\...`.
2. `d3dx9_25.dll`. Extract it from the game's own
   `Support/DirectX9/Apr2005_d3dx9_25_x86.cab` and set a `native,builtin` override —
   running the bundled `DXSETUP.exe` under Wine is unreliable.

**The console is whatever terminal launched the mod.** `DllMain` calls `AllocConsole()`,
but it returns `FALSE` because the game inherits the launcher's console instead of getting
its own. There is no separate console window. Launch `EgoMP.exe` from a real terminal with
a live tty, and keep that shell alive without reading stdin while the game runs, otherwise
the mod has nowhere to read the port and IP from.

To confirm injection actually worked, check that the DLL is mapped into the process rather
than trusting the launcher's exit code — it returns 0 whether or not `LoadLibraryW`
succeeded inside the target:

```sh
lsof -p "$(pgrep -f 'Fable\.exe')" | grep -i egomp
```

---

## Building

**Windows only.** This is an MSVC solution (`EgoMP.sln`, PlatformToolset v143 / VS2022)
that links prebuilt Win32 static libraries — `libMinHook.x86.lib` and
`SLikeNet_LibStatic_Release_Win32.lib`. There is no macOS or Linux build path: the vendored
SLikeNet library is MSVC-compiled C++, so the ABI rules out linking it with MinGW or clang.

Open the solution and build **Release | x86** (the game is 32-bit — an x64 build produces a
DLL that cannot be injected into it).

For development from a non-Windows machine, CI builds the same configuration on a
`windows-latest` runner and uploads `EgoMP.dll` + `EgoMP.exe` as an artifact.

---

## License

GPL-3.0, inherited from the upstream project — see [LICENSE](LICENSE). Derivative works
must stay open source under the same license.

Fable: The Lost Chapters and all related assets belong to their respective owners,
including Microsoft and Lionhead Studios.
