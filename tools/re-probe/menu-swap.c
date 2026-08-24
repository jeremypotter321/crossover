/*
 * menu-swap: change a menu row's label before the row is constructed.
 *
 * There is no hot reload. A CText's string is baked into the component when the
 * component is built, so an edit that lands after the menu exists cannot show
 * until the screen is rebuilt. The edit has to happen in the window before
 * construction -- the same one-instruction window section 13 used to add a
 * seventh row.
 *
 * Editing the resolved text is not usable there. The localised strings do not
 * exist yet: a probe watching from process start never finds the wide "Quit"
 * at all until the frontend loads, which is the very thing we are trying to get
 * in front of. Definitions, by contrast, are loaded long before.
 *
 * So this changes which CText a row is built from, rather than what any string
 * says. Quit's row is:
 *
 *     button 251 -> middle 252 -> children [254 (its CText), 73 (row art)]
 *
 * and at the moment definition 252 is dispatched, its child list is replaced
 * with [269, 73] -- 269 being the CText that Options' row uses. If the last row
 * then reads "Options", a row's label is ours to choose at construction, by the
 * same child-vector mechanism already proved for adding a row.
 *
 * Nothing else is touched: one definition, one vector, one run.
 */

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#define LOG_PATH "menu-swap.log"

#define DEF_ID          0x20
#define DEF_TYPE        0x3C
#define DEF_CHILD_BEGIN 0x70
#define DEF_CHILD_END   0x74
#define DEF_CHILD_CAP   0x78

#define FACTORY_TYPE_READ 0x0041D249u
#define FACTORY_TYPE_LEN  3

#define QUIT_MIDDLE   252     /* the CChangingStateComponent under Quit  */
#define QUIT_CTEXT    254     /* what it uses now                        */
#define OPTIONS_CTEXT 269     /* what we give it instead                 */

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

/* Reserved up front: the handler runs inside the game's own dispatch and must
 * not allocate or call anything that can block. */
static DWORD g_slot[8];
static volatile LONG g_done;
static volatile DWORD g_hit_def;
static volatile DWORD g_old0, g_old1, g_n;
static volatile LONG g_reported;

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

static void swap_child(DWORD def)
{
    DWORD b, e, n, k;

    if (g_done) return;
    if (!PLAUSIBLE(def)) return;
    if (*(DWORD *)(uintptr_t)(def + DEF_ID) != QUIT_MIDDLE) return;

    b = *(DWORD *)(uintptr_t)(def + DEF_CHILD_BEGIN);
    e = *(DWORD *)(uintptr_t)(def + DEF_CHILD_END);
    if (!PLAUSIBLE(b) || e <= b) return;
    n = (e - b) / 4;
    if (n == 0 || n > 8) return;

    for (k = 0; k < n; k++) {
        DWORD id = *(DWORD *)(uintptr_t)(b + k * 4);
        g_slot[k] = (id == QUIT_CTEXT) ? OPTIONS_CTEXT : id;
    }

    g_hit_def = def;
    g_n = n;
    g_old0 = *(DWORD *)(uintptr_t)b;
    g_old1 = n > 1 ? *(DWORD *)(uintptr_t)(b + 4) : 0;

    /* The vector is exactly sized, so it is repointed rather than grown. */
    *(DWORD *)(uintptr_t)(def + DEF_CHILD_BEGIN) = (DWORD)(uintptr_t)g_slot;
    *(DWORD *)(uintptr_t)(def + DEF_CHILD_END)   = (DWORD)(uintptr_t)(g_slot + n);
    *(DWORD *)(uintptr_t)(def + DEF_CHILD_CAP)   = (DWORD)(uintptr_t)(g_slot + n);
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

    swap_child(ebx);

    /* `mov eax,[ebx+0x3C]` done here, so the breakpoint never leaves the code
     * and no dispatch in the construction burst is missed. */
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
    plog("=== menu-swap: Quit's row is built from Options' CText ===");
    plog("definition %d: child %d -> %d, applied before construction",
         QUIT_MIDDLE, QUIT_CTEXT, OPTIONS_CTEXT);

    AddVectoredExceptionHandler(1, bp_handler);
    plog("breakpoint at 0x%08X: %s", FACTORY_TYPE_READ,
         arm_bp() ? "armed" : "FAILED");

    for (t = 0; t < 60000; t++) {
        if (g_done && !g_reported) {
            g_reported = 1;
            plog("");
            plog("=== SWAPPED ===");
            plog("  definition %d at 0x%08lX, %lu child(ren)",
                 QUIT_MIDDLE, g_hit_def, g_n);
            plog("  was [%lu %lu], now [%lu %lu]",
                 g_old0, g_old1, g_slot[0], g_n > 1 ? g_slot[1] : 0);
            disarm_bp();
            plog("  breakpoint removed -- read the last row of the menu");
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
