/*
 * menu-key: give a row a label of our own by rewriting its text KEY.
 *
 * Section 15 established that a row's label is chosen at construction, by
 * pointing the row at a different CText definition. That gives another *stock*
 * label. This asks whether an arbitrary one is available.
 *
 * A CText definition names its string by key -- wide text reached through
 * CUIDef +0x54, e.g. "TEXT_GUI_MENU_QUIT" -- and the key is resolved through
 * the language bank when the component is built. So the question is what the
 * game does with a key that is not in the bank. Engines of this age commonly
 * fall back to rendering the key itself, which would hand us arbitrary text for
 * nothing. If instead it renders nothing, that is equally worth knowing: it
 * says the label must come from a hook on the lookup, and this run is the
 * evidence for that rather than a guess.
 *
 * The key is rewritten IN PLACE, wide, over its own buffer: L"Mods" plus a
 * terminator is 5 wide characters where "TEXT_GUI_MENU_QUIT" has 19, so nothing
 * grows and no allocation is needed. If the CharString carries a separate
 * length it will still be the original 18, so a reader that trusts the length
 * rather than the NUL is the one case this cannot cover -- and the screen will
 * say so.
 *
 * Written in the factory window at 0x0041D249, when definition 254 (Quit's
 * CText) is dispatched and before its component exists. By then the language
 * bank is loaded, which is what makes this the right moment and not earlier.
 */

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#define LOG_PATH "menu-key.log"

#define DEF_ID   0x20
#define DEF_KEY  0x54          /* -> CharString -> wide key */
#define DEF_TYPE 0x3C

#define FACTORY_TYPE_READ 0x0041D249u
#define FACTORY_TYPE_LEN  3

#define QUIT_CTEXT 254

static const WCHAR OURS[] = L"Mods";

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

#define PLAUSIBLE(p) ((p) >= 0x00400000u && (p) < 0x20000000u)

static unsigned char g_orig;
static int g_saved;

static volatile LONG g_done, g_reported;
static volatile DWORD g_def, g_strobj, g_chars;
static char g_was[128];

static int arm_bp(void)
{
    DWORD prot;
    unsigned char cur = *(unsigned char *)(uintptr_t)FACTORY_TYPE_READ;
    if (cur == 0xCC) return 1;
    if (!g_saved) { g_orig = cur; g_saved = 1; }
    if (!VirtualProtect((void *)(uintptr_t)FACTORY_TYPE_READ, 1,
                        PAGE_EXECUTE_READWRITE, &prot)) return 0;
    *(unsigned char *)(uintptr_t)FACTORY_TYPE_READ = 0xCC;
    VirtualProtect((void *)(uintptr_t)FACTORY_TYPE_READ, 1, prot, &prot);
    return 1;
}

static void disarm_bp(void)
{
    DWORD prot;
    if (!g_saved) return;
    if (VirtualProtect((void *)(uintptr_t)FACTORY_TYPE_READ, 1,
                       PAGE_EXECUTE_READWRITE, &prot)) {
        *(unsigned char *)(uintptr_t)FACTORY_TYPE_READ = g_orig;
        VirtualProtect((void *)(uintptr_t)FACTORY_TYPE_READ, 1, prot, &prot);
    }
}

static void rewrite_key(DWORD def)
{
    DWORD strobj, chars;
    int i;

    if (g_done) return;
    if (!PLAUSIBLE(def)) return;
    if (*(DWORD *)(uintptr_t)(def + DEF_ID) != QUIT_CTEXT) return;

    strobj = *(DWORD *)(uintptr_t)(def + DEF_KEY);
    if (!PLAUSIBLE(strobj)) return;
    chars = *(DWORD *)(uintptr_t)strobj;
    if (!PLAUSIBLE(chars)) return;

    /* Keep the original for the log; it is wide, so take every other byte. */
    for (i = 0; i < 63; i++) {
        unsigned short w = *(unsigned short *)(uintptr_t)(chars + i * 2);
        if (!w || w < 0x20 || w > 0x7E) break;
        g_was[i] = (char)w;
        g_was[i + 1] = 0;
    }

    g_def = def; g_strobj = strobj; g_chars = chars;

    /* In place, wide, shorter than what is there -- and with NO VirtualProtect.
     *
     * The first version of this called VirtualProtect here and the game hung
     * black with the breakpoint armed and never hit: this runs inside the
     * game's own component dispatch, and taking a loader/heap lock from there
     * deadlocks it. menu-swap.c survives the same window precisely because it
     * only writes plain memory. The key characters are heap data and already
     * writable, so the memcpy needs nothing. */
    memcpy((void *)(uintptr_t)chars, OURS, sizeof OURS);
    g_done = 1;
}

static LONG CALLBACK bp_handler(EXCEPTION_POINTERS *ep)
{
    DWORD ebx;

    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT ||
        (DWORD)(uintptr_t)ep->ExceptionRecord->ExceptionAddress != FACTORY_TYPE_READ)
        return EXCEPTION_CONTINUE_SEARCH;

    ebx = ep->ContextRecord->Ebx;
    if (!PLAUSIBLE(ebx)) {
        disarm_bp();
        ep->ContextRecord->Eip = FACTORY_TYPE_READ;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    rewrite_key(ebx);

    ep->ContextRecord->Eax = *(DWORD *)(uintptr_t)(ebx + DEF_TYPE);
    ep->ContextRecord->Eip = FACTORY_TYPE_READ + FACTORY_TYPE_LEN;
    return EXCEPTION_CONTINUE_EXECUTION;
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
    int t;

    (void)unused;
    g_log = fopen(LOG_PATH, "w");
    plog("=== menu-key: Quit's text key rewritten to \"Mods\" ===");
    plog("in the factory window, before the component is built");

    AddVectoredExceptionHandler(1, bp_handler);
    plog("breakpoint at 0x%08X: %s", FACTORY_TYPE_READ,
         arm_bp() ? "armed" : "FAILED");

    for (t = 0; t < 60000; t++) {
        if (g_done && !g_reported) {
            g_reported = 1;
            plog("");
            plog("=== KEY REWRITTEN ===");
            plog("  CText def %d at 0x%08lX", QUIT_CTEXT, g_def);
            plog("  CharString 0x%08lX -> characters 0x%08lX", g_strobj, g_chars);
            plog("  was \"%s\", now \"Mods\"", g_was);
            disarm_bp();
            plog("  read the last row: \"Mods\" means the game falls back to");
            plog("  the key; blank means the lookup must be hooked instead");
        }
        if (!g_done && (t & 0x1F) == 0) dismiss_dialogs();
        Sleep(20);
    }
    disarm_bp();
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
