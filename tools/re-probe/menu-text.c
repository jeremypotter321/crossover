/*
 * menu-text: prove what the per-button id at CUIDef+0xC4 actually selects.
 *
 * menu-inspect established that a CFrontEndButton definition contains no text
 * at all -- not one string is reachable from any of the six main-menu buttons,
 * which is what a localised game looks like: labels live in a per-language
 * bank and the definition only names an index into it.
 *
 * Diffing the six same-type definitions left exactly one pair of semantic
 * scalars that differs per button and is not an object id or a pointer:
 *
 *   offset   Continue  Change  Options  Credits  About  Quit
 *   +0x0C4        66      16      297       67    321    314
 *   +0x0E4        66      16      297       67    321    314    (mirror)
 *
 * Two readings fit equally well and they are told apart by one experiment.
 * Either it selects the button's TEXT, or it selects the button's ACTION. So
 * this takes the value that means Options (297) and writes it over Quit's
 * (314), in the window before the button is constructed, and the menu is then
 * simply read:
 *
 *   the last row now says "Options"      -> +0xC4 is the text id
 *   the last row still says "Quit" but
 *     opens the Options screen           -> +0xC4 is the action
 *   both                                 -> it is the button's whole identity,
 *                                           which is the best possible outcome
 *   neither                              -> the field is something else
 *
 * Only Quit's definition is touched. Nothing else in the game is modified, so
 * anything that changes on screen is attributable to this one write.
 */

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#define LOG_PATH "menu-text.log"

#define DEF_ID    0x20
#define DEF_TYPE  0x3C
#define DEF_WHAT  0xC4      /* the per-button id under test */
#define DEF_WHAT2 0xE4      /* its mirror */

#define TYPE_BUTTON 0x0B

#define FACTORY_TYPE_READ 0x0041D249u
#define FACTORY_TYPE_LEN  3

#define QUIT_ID     251
#define QUIT_WHAT   314     /* what Quit ships with    */
#define OPTIONS_WHAT 297    /* what Options ships with */

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

static volatile DWORD g_hit_def;
static volatile DWORD g_was, g_was2;
static volatile LONG  g_done;
static volatile LONG  g_reported;

static int arm_bp(void)
{
    DWORD prot;
    unsigned char cur = *(unsigned char *)(uintptr_t)FACTORY_TYPE_READ;

    if (cur == 0xCC) return 1;
    if (!g_saved) { g_orig = cur; g_saved = 1; }
    if (!VirtualProtect((void *)(uintptr_t)FACTORY_TYPE_READ, 1,
                        PAGE_EXECUTE_READWRITE, &prot))
        return 0;
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

static void retext(DWORD def)
{
    if (g_done) return;
    if (!PLAUSIBLE(def)) return;
    if (*(DWORD *)(uintptr_t)(def + DEF_TYPE) != TYPE_BUTTON) return;
    if (*(DWORD *)(uintptr_t)(def + DEF_ID) != QUIT_ID) return;

    g_hit_def = def;
    g_was  = *(DWORD *)(uintptr_t)(def + DEF_WHAT);
    g_was2 = *(DWORD *)(uintptr_t)(def + DEF_WHAT2);

    *(DWORD *)(uintptr_t)(def + DEF_WHAT)  = OPTIONS_WHAT;
    *(DWORD *)(uintptr_t)(def + DEF_WHAT2) = OPTIONS_WHAT;
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

    retext(ebx);

    ep->ContextRecord->Eax = *(DWORD *)(uintptr_t)(ebx + DEF_TYPE);
    ep->ContextRecord->Eip = FACTORY_TYPE_READ + FACTORY_TYPE_LEN;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static DWORD WINAPI worker(LPVOID unused)
{
    int t;

    (void)unused;
    g_log = fopen(LOG_PATH, "w");
    plog("=== menu-text: is CUIDef+0xC4 the text or the action? ===");
    plog("Quit (def id %d) gets Options' value: %d -> %d",
         QUIT_ID, QUIT_WHAT, OPTIONS_WHAT);
    plog("nothing else in the game is modified");

    AddVectoredExceptionHandler(1, bp_handler);
    plog("breakpoint at 0x%08X: %s", FACTORY_TYPE_READ,
         arm_bp() ? "armed" : "FAILED");

    for (t = 0; t < 100000; t++) {
        if (g_done && !g_reported) {
            g_reported = 1;
            plog("");
            plog("=== WRITTEN ===");
            plog("  Quit definition 0x%08lX", g_hit_def);
            plog("  +0x%02X was %lu, now %d", DEF_WHAT, g_was, OPTIONS_WHAT);
            plog("  +0x%02X was %lu, now %d", DEF_WHAT2, g_was2, OPTIONS_WHAT);
            disarm_bp();
            plog("  breakpoint removed -- read the last row of the menu");
        }
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
