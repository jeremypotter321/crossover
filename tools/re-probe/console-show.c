/*
 * console-show: make Lionhead's dev console draw.
 *
 * The console's renderer is still wired into the retail frame loop. At
 * 0x00435C19 the frame function reads `CConsole+0x78` and, only if it is set,
 * calls the console's draw at 0x009EA550:
 *
 *     0x00435C13  mov ecx, [0x13CAA40]     ; the CConsole singleton
 *     0x00435C19  mov al,  [ecx+0x78]      ; the "console is open" flag
 *     0x00435C1C  test al, al
 *     0x00435C1E  je   0x00435C5A          ; <- always taken in retail
 *     ...                                  ; two floats from .rdata
 *     0x00435C55  call 0x009EA550           ; CConsole::Draw
 *
 * The same flag is what the orphaned `OpenConsole` at 0x006344C0 sets, so this
 * is the engine's own switch, not a guess.
 *
 * That makes showing the console a ONE BYTE WRITE, with no call into game code,
 * no patched instructions and no breakpoints -- the three things that have
 * crashed a live session on this project. The flag is re-asserted for a while
 * in case the engine clears it on a state change.
 *
 * This only makes the console DRAW. Typing into it additionally needs the input
 * processor in manager slot +0x1F8 engaged, which is the other half of
 * OpenConsole and does require a call on the game thread.
 *
 * Build:  make SRC=console-show.c attach
 * Undo:   relaunch the game (nothing on disk is modified).
 */

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>

#define LOG_PATH "C:\\Games\\Fable\\console-show.log"

#define CONSOLE_SINGLETON 0x013CAA40u
#define VT_CONSOLE        0x0129C600u
#define OPEN_FLAG         0x78          /* CConsole+0x78 */
#define ALPHA             0x7C          /* ConsoleAlpha's storage, seen = 0xFF */

/* Build with -DCONSOLE_OPEN=0 to put the flag back. Clearing it matters: the
 * input router at 0x006880F7 reads the same byte and, when it is set, SKIPS a
 * branch of normal input handling. Leaving it on after finding the draw is a
 * stub buys nothing and risks swallowing input. */
#ifndef CONSOLE_OPEN
#define CONSOLE_OPEN 1
#endif

static FILE *g_log;

static void plog(const char *fmt, ...)
{
    va_list ap;
    if (!g_log) return;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

static int readable(DWORD p)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!p) return 0;
    if (VirtualQuery((void *)(uintptr_t)p, &mbi, sizeof mbi) != sizeof mbi)
        return 0;
    return mbi.State == MEM_COMMIT &&
           !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS));
}
static DWORD rd(DWORD p) { return readable(p) ? *(DWORD *)(uintptr_t)p : 0; }

static DWORD WINAPI probe_main(LPVOID unused)
{
    DWORD c;
    int i, was;
    (void)unused;

    g_log = fopen(LOG_PATH, "w");
    if (!g_log) return 0;
    plog("=== console-show: set CConsole+0x78 so the frame loop draws it ===");

    Sleep(2000);

    c = rd(CONSOLE_SINGLETON);
    if (!c || rd(c) != VT_CONSOLE) {
        plog("CConsole not up (*0x%08X = 0x%08lX, vtable 0x%08lX) -- doing nothing",
             CONSOLE_SINGLETON, c, rd(c));
        fclose(g_log);
        return 0;
    }
    plog("CConsole @0x%08lX  vtable ok", c);

    if (!readable(c + OPEN_FLAG)) {
        plog("+0x%02X unreadable -- doing nothing", OPEN_FLAG);
        fclose(g_log);
        return 0;
    }

    was = *(unsigned char *)(uintptr_t)(c + OPEN_FLAG);
    plog("before: +0x%02X (open) = %d, +0x%02X (alpha) = %lu",
         OPEN_FLAG, was, ALPHA, rd(c + ALPHA));

    /* The write. One byte, in the game's own data, that the frame loop reads
     * each frame -- nothing is executing here that the engine does not already
     * execute every frame once the flag is set. */
    *(unsigned char *)(uintptr_t)(c + OPEN_FLAG) = CONSOLE_OPEN;
    plog("after:  +0x%02X (open) = %d", OPEN_FLAG,
         *(unsigned char *)(uintptr_t)(c + OPEN_FLAG));

    if (!CONSOLE_OPEN) {
        plog("=== done (flag cleared) ===");
        fclose(g_log);
        g_log = NULL;
        return 0;
    }

    /* Hold it on. If something clears it, say so rather than fighting silently:
     * whatever clears it is the next thing worth reading. */
    for (i = 0; i < 120; i++) {
        Sleep(1000);
        if (!readable(c + OPEN_FLAG)) { plog("t=%ds: CConsole went away", i); break; }
        if (*(unsigned char *)(uintptr_t)(c + OPEN_FLAG) == 0) {
            plog("t=%ds: something CLEARED +0x%02X -- re-asserting", i, OPEN_FLAG);
            *(unsigned char *)(uintptr_t)(c + OPEN_FLAG) = 1;
        }
    }

    plog("=== done (flag left set; relaunch to undo) ===");
    fclose(g_log);
    g_log = NULL;
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        CloseHandle(CreateThread(NULL, 0, probe_main, NULL, 0, NULL));
    }
    return TRUE;
}
