/*
 * menu-mods: make a main-menu row say "Mods".
 *
 * The label chain, now fully established:
 *
 *   button (11) -> CChangingStateComponent (5) -> CText (6)
 *   CText CUIDef +0x54          -> wide "TEXT_GUI_MENU_QUIT"   (the key)
 *   CUIStateDef  +0x20 (x5)     -> the same key, per state
 *   ...and elsewhere in the heap, the resolved wide text        "Quit"
 *
 * Both are UTF-16, which is why every earlier probe reported "no strings
 * reachable": the reader stopped at the first NUL and threw the string away as
 * one character of noise.
 *
 * The key would need a matching entry in the language bank to be worth
 * changing, so this changes the resolved text instead -- and "Quit" is exactly
 * as long as "Mods", so the replacement is in place, needs no allocation, and
 * cannot overrun the buffer it sits in.
 *
 * Done before the frontend is built, so the first menu drawn is already the
 * answer rather than something that needs a rebuild to show.
 */

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#define LOG_PATH "menu-mods.log"

static const WCHAR FROM[] = L"Quit";
static const WCHAR TO[]   = L"Mods";

static FILE *g_log;
static DWORD g_self_lo, g_self_hi;

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

#define IS_SELF(p) ((p) >= g_self_lo && (p) < g_self_hi)

static int readable(const MEMORY_BASIC_INFORMATION *mbi)
{
    DWORD p = mbi->Protect;
    if (mbi->State != MEM_COMMIT) return 0;
    if (p & (PAGE_GUARD | PAGE_NOACCESS)) return 0;
    /* Writable only -- the copy we want to edit is heap data, and skipping
     * read-only pages keeps us off the executable's own constants. */
    return (p & (PAGE_READWRITE | PAGE_WRITECOPY |
                 PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

#define MAX_HITS 64
static DWORD g_hits[MAX_HITS];
static int g_nhits;

static void find_all(void)
{
    MEMORY_BASIC_INFORMATION mbi;
    unsigned char *addr = (unsigned char *)0x00010000;
    unsigned char *limit = (unsigned char *)0x7FFF0000;
    int n = (int)(sizeof FROM / sizeof FROM[0]) - 1;   /* chars, no NUL */

    g_nhits = 0;
    while (addr < limit && VirtualQuery(addr, &mbi, sizeof mbi) == sizeof mbi) {
        unsigned char *base = (unsigned char *)mbi.BaseAddress;
        SIZE_T size = mbi.RegionSize, off;

        if (readable(&mbi) && size >= 0x100 && !IS_SELF((DWORD)(uintptr_t)base)) {
            for (off = 0; off + (SIZE_T)(n * 2) + 2 <= size; off += 2) {
                if (memcmp(base + off, FROM, n * 2) != 0) continue;
                if (*(unsigned short *)(base + off + n * 2) != 0) continue;
                if (g_nhits < MAX_HITS)
                    g_hits[g_nhits++] = (DWORD)(uintptr_t)(base + off);
            }
        }
        addr = base + size;
        if (size == 0) break;
    }
}

static int apply(void)
{
    int i, wrote = 0;
    int n = (int)(sizeof FROM / sizeof FROM[0]) - 1;

    for (i = 0; i < g_nhits; i++) {
        DWORD prot;
        void *p = (void *)(uintptr_t)g_hits[i];
        if (memcmp(p, TO, n * 2) == 0) continue;      /* already ours */
        if (!VirtualProtect(p, n * 2, PAGE_READWRITE, &prot)) continue;
        memcpy(p, TO, n * 2);
        VirtualProtect(p, n * 2, prot, &prot);
        wrote++;
    }
    return wrote;
}

static void dismiss_dialogs(void)
{
    HWND hw = FindWindowA(NULL, "Fable - The Lost Chapters ");
    if (!hw) hw = FindWindowA(NULL, "Fable - The Lost Chapters");
    if (!hw) return;
    PostMessageA(hw, WM_KEYDOWN, VK_RETURN, 0);
    PostMessageA(hw, WM_KEYUP,   VK_RETURN, 0);
}

static DWORD WINAPI worker(LPVOID unused)
{
    MEMORY_BASIC_INFORMATION mbi;
    int t, announced = 0;

    (void)unused;
    g_log = fopen(LOG_PATH, "w");
    if (VirtualQuery((void *)worker, &mbi, sizeof mbi) == sizeof mbi) {
        g_self_lo = (DWORD)(uintptr_t)mbi.AllocationBase;
        g_self_hi = g_self_lo + 0x200000;
    }
    plog("=== menu-mods: \"Quit\" -> \"Mods\" ===");
    plog("wide, in place, same length; this DLL (0x%08lX..) excluded",
         g_self_lo);

    /* The text bank is loaded before the frontend is built, but not
     * instantly -- so keep looking until it is there, then keep it ours in
     * case the row is rebuilt from the bank later. */
    for (t = 0; t < 20000; t++) {
        int wrote;
        find_all();
        wrote = apply();
        if (wrote && !announced) {
            int i;
            plog("");
            plog("=== rewritten: %d occurrence(s) ===", wrote);
            for (i = 0; i < g_nhits && i < 12; i++)
                plog("    0x%08lX", g_hits[i]);
            announced = 1;
        }
        /* Keep dismissing regardless of whether a write has landed: the
         * first matches come from a system module's own string table, so
         * `announced` goes true long before the game reaches its menu, and
         * gating the dismissal on it leaves the video dialog up forever. */
        if (t < 480 && (t & 0xF) == 0) dismiss_dialogs();
        Sleep(250);
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        CreateThread(NULL, 0, worker, NULL, 0, NULL);
    }
    return TRUE;
}
