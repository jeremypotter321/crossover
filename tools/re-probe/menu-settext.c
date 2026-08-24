/*
 * menu-settext: test whether CText CUIDef+0x38 selects a menu row's string.
 *
 * What the diffs left standing. A menu row's label is a CText definition two
 * levels below the button:
 *
 *     button (type 11) -> CChangingStateComponent (type 5) -> CText (type 6)
 *
 * No definition anywhere in that chain contains a string, so the label is
 * referenced by id. Across the six rows' CText definitions only one field is
 * both distinct for every row and the right shape for a string table:
 *
 *     +0x38   Continue 33129   Change 17377   Options  3017
 *             Credits  65073   About  50457   Quit    36145
 *
 * All six differ and all six fit in 16 bits, which random data would not.
 * Two other per-row scalars were eliminated by the data itself rather than by
 * experiment: +0x14 is shared by Options and Credits, and +0x18 is shared by
 * Continue Game and Quit, so neither can select a unique string.
 *
 * The test writes Options' value over Quit's. If the last row then reads
 * "Options", +0x38 is the text id and a row's label becomes ours to set.
 *
 * This edits the definition, not the built component, so it takes effect the
 * next time the menu is constructed -- leave the menu and come back. That is
 * also why it is worth doing this way: a definition edit is what an appended
 * row would inherit.
 */

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#define LOG_PATH "menu-settext.log"

#define CUIDEF_VTABLE 0x01259F8Cu
#define DEF_ID    0x20
#define DEF_TEXT  0x38          /* the field under test */
#define DEF_TYPE  0x3C

#define QUIT_TEXT_DEF     254
#define OPTIONS_TEXT_DEF  269

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

static int readable(const MEMORY_BASIC_INFORMATION *mbi)
{
    DWORD p = mbi->Protect;
    if (mbi->State != MEM_COMMIT) return 0;
    if (p & (PAGE_GUARD | PAGE_NOACCESS)) return 0;
    return (p & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                 PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                 PAGE_EXECUTE_WRITECOPY)) != 0;
}

/* Every CUIDef starts with the same vtable and carries its id at +0x20, so one
 * pass over committed memory finds any definition by id with no timing, no
 * breakpoint and nothing called in the game. */
static DWORD find_def(DWORD id)
{
    MEMORY_BASIC_INFORMATION mbi;
    unsigned char *addr = (unsigned char *)0x00010000;
    unsigned char *limit = (unsigned char *)0x7FFF0000;

    while (addr < limit && VirtualQuery(addr, &mbi, sizeof mbi) == sizeof mbi) {
        unsigned char *base = (unsigned char *)mbi.BaseAddress;
        SIZE_T size = mbi.RegionSize, off;

        if (readable(&mbi) && size >= 0x200)
            for (off = 0; off + 0x200 <= size; off += 4) {
                DWORD *p = (DWORD *)(base + off);
                if (p[0] == CUIDEF_VTABLE && p[DEF_ID / 4] == id)
                    return (DWORD)(uintptr_t)p;
            }
        addr = base + size;
        if (size == 0) break;
    }
    return 0;
}

static DWORD WINAPI worker(LPVOID unused)
{
    DWORD quit, options, want, had;
    int t;

    (void)unused;
    g_log = fopen(LOG_PATH, "w");
    plog("=== menu-settext: is CText CUIDef+0x38 the row's text id? ===");

    quit    = find_def(QUIT_TEXT_DEF);
    options = find_def(OPTIONS_TEXT_DEF);
    plog("Quit's CText    (id %d) def 0x%08lX", QUIT_TEXT_DEF, quit);
    plog("Options' CText  (id %d) def 0x%08lX", OPTIONS_TEXT_DEF, options);
    if (!quit || !options) { plog("not found -- nothing done"); return 0; }

    want = *(DWORD *)(uintptr_t)(options + DEF_TEXT);
    had  = *(DWORD *)(uintptr_t)(quit + DEF_TEXT);
    plog("");
    plog("+0x%02X: Quit has %lu, Options has %lu", DEF_TEXT, had, want);

    *(DWORD *)(uintptr_t)(quit + DEF_TEXT) = want;
    plog("written -- Quit's CText +0x%02X is now %lu", DEF_TEXT,
         *(DWORD *)(uintptr_t)(quit + DEF_TEXT));
    plog("");
    plog("The menu on screen was built before this write, so it will not");
    plog("change until it is rebuilt: go into Options (or Credits) and come");
    plog("back, then read the last row.");

    /* Hold the value: if anything reloads the definition, put it back, so a
     * rebuild cannot quietly restore the original and look like a null result. */
    for (t = 0; t < 30000; t++) {
        if (*(DWORD *)(uintptr_t)(quit + DEF_TEXT) != want) {
            *(DWORD *)(uintptr_t)(quit + DEF_TEXT) = want;
            plog("(value had been reset -- rewritten)");
        }
        Sleep(100);
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
