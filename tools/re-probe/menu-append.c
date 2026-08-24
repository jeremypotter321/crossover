/*
 * menu-append: put a seventh, NATIVE entry into Fable's main menu.
 *
 * Everything before this drew *over* the menu. This puts an entry *in* it, by
 * the only mechanism that actually drives what a screen renders.
 *
 * The finding this is built on (docs/ui-system.md section 12): a screen does
 * not have components pushed into it. It builds its own children at
 * construction from a list of definition ids inside its own definition:
 *
 *   CUIDef +0x70  begin    std::vector<uint32> of child definition ids
 *          +0x74  end
 *          +0x78  capacity
 *
 * So the entire job is: get at the main menu's definition in the window after
 * it is resolved and before its children are built, and append one id.
 *
 * That window is exactly one instruction. At 0x0041D249 the factory does
 * `mov eax,[ebx+0x3C]` with EBX already holding the resolved definition, and
 * the component has not been built yet.
 *
 * An INT3 there is used rather than the inline trampoline the previous probe
 * tried: the trampoline killed the game (a jmp rel32 to a stub VirtualAlloc
 * may place more than 2GB away silently overflows), while Wine delivers
 * EXCEPTION_BREAKPOINT reliably -- it is how every live fact in the docs was
 * established.
 *
 * Nothing here calls into the game. The first version of this probe resolved
 * button names through the game's own definition manager to identify the main
 * menu, and reaching the manager worked -- but calling GetDefinition on it from
 * our own thread killed the process every time. The game's frontend is being
 * built on its own thread at that moment and the manager is not ours to call
 * re-entrantly.
 *
 * So this identifies nothing by name. It extends EVERY screen definition it
 * sees, by duplicating that screen's own last child id. That needs no lookup,
 * no allocation on the game's thread, and no knowledge of which screen is
 * which -- and the main menu is one of the screens, so it gets its seventh
 * entry along with the rest. Every definition and its child ids are logged, so
 * one run also produces the id table that a targeted version can use.
 */

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#define LOG_PATH "menu-append.log"

/* --- definition layout (docs/ui-system.md sections 8-12) --- */
#define DEF_ID          0x20      /* the handle the factory is driven by     */
#define DEF_TYPE        0x3C      /* component type                          */
#define DEF_CHILD_BEGIN 0x70
#define DEF_CHILD_END   0x74
#define DEF_CHILD_CAP   0x78

/* A screen's own children are containers as often as they are buttons: the
 * main menu's six rows hang off a list (0x0C), not off the screen (0x0A). Both
 * are extended, or the rows are never reached. */
#define TYPE_SCREEN     0x0A
#define TYPE_LIST       0x0C

/* --- the game's own code --- */
#define FACTORY_TYPE_READ 0x0041D249u   /* mov eax,[ebx+0x3C] -- our window   */
#define FACTORY_TYPE_LEN  3             /* 8B 43 3C, no relative operands     */

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

/*
 * Everything the handler touches is reserved up front.
 *
 * The handler runs inside the game's own component dispatch. It allocates
 * nothing, calls nothing that can block, and writes no log -- the previous
 * probe established that doing any of those from in here is what kills the
 * process. Findings are recorded into fixed storage and printed by the worker.
 */
#define POOL_SLOTS 128
#define POOL_ELEMS 64
static DWORD *g_pool;
static volatile LONG g_pool_next;

#define MAX_DEFS 32
typedef struct {
    DWORD def;
    DWORD id;
    DWORD type;
    DWORD n;                 /* children before the append */
    DWORD ids[POOL_ELEMS];
    LONG  patched;
} SeenDef;

static SeenDef g_seen[MAX_DEFS];
static volatile LONG g_seen_n;
static volatile LONG g_reported;

#define PLAUSIBLE(p) ((p) >= 0x00400000u && (p) < 0x20000000u)

static unsigned char g_type_orig;
static int g_type_saved;

static int arm_bp(DWORD site, unsigned char *orig, int *saved)
{
    DWORD prot;
    unsigned char cur = *(unsigned char *)(uintptr_t)site;

    if (cur == 0xCC) return 1;
    if (!*saved) { *orig = cur; *saved = 1; }
    if (!VirtualProtect((void *)(uintptr_t)site, 1, PAGE_EXECUTE_READWRITE, &prot))
        return 0;
    *(unsigned char *)(uintptr_t)site = 0xCC;
    VirtualProtect((void *)(uintptr_t)site, 1, prot, &prot);
    return 1;
}

static void disarm_bp(DWORD site, unsigned char orig)
{
    DWORD prot;
    if (VirtualProtect((void *)(uintptr_t)site, 1, PAGE_EXECUTE_READWRITE, &prot)) {
        *(unsigned char *)(uintptr_t)site = orig;
        VirtualProtect((void *)(uintptr_t)site, 1, prot, &prot);
    }
}

/* Is this vector one we already replaced? The pool is contiguous, so the test
 * is a range check -- cheaper and more certain than comparing contents. */
static int in_pool(DWORD p)
{
    DWORD lo = (DWORD)(uintptr_t)g_pool;
    DWORD hi = lo + POOL_SLOTS * POOL_ELEMS * 4;
    return p >= lo && p < hi;
}

/*
 * The append.
 *
 * The vector is exactly sized (end == capacity), so it cannot grow in place --
 * the replacement is a pool slot holding a copy plus one more id, and the
 * three pointers are repointed at it. The definition outlives the screen, so
 * the copy has to outlive it too; the pool is never freed.
 *
 * The appended id is the screen's own last child, duplicated. A stock id means
 * the game builds the extra child with its own machinery, exactly as it builds
 * the other six.
 */
static void extend_screen(DWORD def)
{
    DWORD b, e, n, k, *nv, type;
    LONG slot, idx;

    if (!g_pool) return;
    if (!PLAUSIBLE(def)) return;
    type = *(DWORD *)(uintptr_t)(def + DEF_TYPE);
    if (type != TYPE_SCREEN && type != TYPE_LIST) return;

    b = *(DWORD *)(uintptr_t)(def + DEF_CHILD_BEGIN);
    e = *(DWORD *)(uintptr_t)(def + DEF_CHILD_END);
    if (in_pool(b)) return;                       /* already extended */
    if (!PLAUSIBLE(b) || e <= b || (e - b) > 0x200) return;
    n = (e - b) / 4;
    if (n == 0 || n + 1 >= POOL_ELEMS) return;

    slot = g_pool_next;
    idx  = g_seen_n;
    if (slot >= POOL_SLOTS || idx >= MAX_DEFS) return;
    g_pool_next = slot + 1;
    g_seen_n = idx + 1;

    nv = g_pool + (DWORD)slot * POOL_ELEMS;
    for (k = 0; k < n; k++) nv[k] = *(DWORD *)(uintptr_t)(b + k * 4);
    nv[n] = nv[n - 1];                            /* duplicate the last child */

    /* Recorded for the log; the worker prints it once it is safe to. */
    g_seen[idx].def  = def;
    g_seen[idx].id   = *(DWORD *)(uintptr_t)(def + DEF_ID);
    g_seen[idx].type = type;
    g_seen[idx].n    = n;
    for (k = 0; k < n; k++) g_seen[idx].ids[k] = nv[k];

    *(DWORD *)(uintptr_t)(def + DEF_CHILD_BEGIN) = (DWORD)(uintptr_t)nv;
    *(DWORD *)(uintptr_t)(def + DEF_CHILD_END)   = (DWORD)(uintptr_t)(nv + n + 1);
    *(DWORD *)(uintptr_t)(def + DEF_CHILD_CAP)   = (DWORD)(uintptr_t)(nv + n + 1);

    g_seen[idx].patched = 1;
}

/*
 * The handler emulates the instruction instead of restoring it.
 *
 * The obvious shape -- put the original byte back, point EIP at it, let the
 * worker re-arm -- leaves the breakpoint absent for as long as it takes the
 * worker to notice. Components are built in a burst of hundreds of factory
 * calls inside a few milliseconds, so that shape samples a handful and misses
 * the rest. The first run of this probe caught four definitions and the main
 * menu was not among them.
 *
 * There is no need to re-run the instruction at all. It is `mov eax,[ebx+0x3C]`
 * -- three bytes, no relative operand, and `mov` writes no flags. Performing it
 * here and resuming past it means the INT3 never has to leave the code, so
 * every single factory call is seen and there is no window to race with.
 */
static LONG CALLBACK bp_handler(EXCEPTION_POINTERS *ep)
{
    DWORD ebx;

    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT ||
        (DWORD)(uintptr_t)ep->ExceptionRecord->ExceptionAddress != FACTORY_TYPE_READ)
        return EXCEPTION_CONTINUE_SEARCH;

    ebx = ep->ContextRecord->Ebx;

    if (!PLAUSIBLE(ebx)) {
        /* Not ours to emulate -- hand it back to the game unchanged and let it
         * fault exactly where it would have. */
        disarm_bp(FACTORY_TYPE_READ, g_type_orig);
        ep->ContextRecord->Eip = FACTORY_TYPE_READ;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    extend_screen(ebx);

    ep->ContextRecord->Eax = *(DWORD *)(uintptr_t)(ebx + DEF_TYPE);
    ep->ContextRecord->Eip = FACTORY_TYPE_READ + FACTORY_TYPE_LEN;
    return EXCEPTION_CONTINUE_EXECUTION;
}

/* Wine's intermittent 0x80040218 video-playback dialog sits in front of the
 * menu and is what stopped the previous run from ever being photographed.
 * osascript cannot click it (no accessibility permission here) but we are
 * inside the process, so the keystroke goes straight to the game's window. */
static void dismiss_dialogs(void)
{
    HWND hw = FindWindowA(NULL, "Fable - The Lost Chapters ");
    if (!hw) hw = FindWindowA(NULL, "Fable - The Lost Chapters");
    if (!hw) return;
    PostMessageA(hw, WM_KEYDOWN, VK_RETURN, 0);
    PostMessageA(hw, WM_KEYUP,   VK_RETURN, 0);
}

static void report_new(void)
{
    LONG have = g_seen_n;

    while (g_reported < have) {
        SeenDef *s = &g_seen[g_reported];
        char line[512];
        int off = 0;
        DWORD k;

        off += snprintf(line + off, sizeof line - off,
                        "%s def 0x%08lX id %lu: %lu -> %lu children  [",
                        s->type == TYPE_LIST ? "list  " : "screen",
                        s->def, s->id, s->n, s->n + 1);
        for (k = 0; k < s->n && off < (int)sizeof line - 16; k++)
            off += snprintf(line + off, sizeof line - off, "%s%lu",
                            k ? " " : "", s->ids[k]);
        snprintf(line + off, sizeof line - off, "] +%lu",
                 s->n ? s->ids[s->n - 1] : 0);
        plog("%s", line);
        g_reported++;
    }
}

static DWORD WINAPI worker(LPVOID unused)
{
    int t;

    (void)unused;
    g_log = fopen(LOG_PATH, "w");
    plog("=== menu-append: a seventh entry in the real main menu ===");
    plog("no calls into the game; every screen definition gains a duplicate "
         "of its own last child");

    g_pool = (DWORD *)VirtualAlloc(NULL, POOL_SLOTS * POOL_ELEMS * 4,
                                   MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    plog("child-list pool: %s", g_pool ? "allocated" : "FAILED");
    if (!g_pool) return 0;

    AddVectoredExceptionHandler(1, bp_handler);

    /* Armed once and left in place -- the handler emulates the instruction, so
     * there is nothing to re-arm and nothing to miss. */
    plog("breakpoint at 0x%08X: %s", FACTORY_TYPE_READ,
         arm_bp(FACTORY_TYPE_READ, &g_type_orig, &g_type_saved) ? "armed"
                                                                : "FAILED");

    for (t = 0; t < 200000; t++) {
        report_new();
        if ((t & 0x3F) == 0) dismiss_dialogs();
        Sleep(20);
    }

    disarm_bp(FACTORY_TYPE_READ, g_type_orig);
    report_new();
    plog("=== done: %ld screen definition(s) extended ===", g_seen_n);
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
