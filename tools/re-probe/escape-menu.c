/*
 * escape-menu: add a "Crossover" entry to Fable's in-game (Escape) menu.
 *
 * Uses the attachment mechanism documented in docs/ui-system.md #12: a screen
 * builds its children at construction from a vector of definition ids at
 * CUIDef+0x70..+0x78. Appending an id to that vector *before* the screen is
 * constructed makes the screen build one extra component through the game's
 * own machinery. The window is the factory breakpoint at 0x0041D249, where the
 * definition is resolved (EBX) but the component is not yet built.
 *
 * Three things this gets right that cost time before:
 *
 *  - Match on ExceptionRecord->ExceptionAddress, NOT ContextRecord->Eip. An
 *    earlier trap in seal.c matched Eip == addr+1, missed under Wine, returned
 *    CONTINUE_SEARCH and killed the process.
 *  - Save the original byte exactly once. Re-arming while already armed stores
 *    0xCC as the "original" and corrupts the restore.
 *  - Pre-allocate the replacement vectors and leave the code page writable, so
 *    the handler makes no Win32 call on the game's dispatch path.
 *
 * A duplicated component is invisible -- it inherits its twin's coordinates and
 * draws exactly on top -- so the injected entry is moved via its definition's
 * own x/y at CUIDef+0x58/+0x5C, which is how the "Hello World" text was made
 * visible without needing the still-unidentified component X field.
 */

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#define LOG_PATH "C:\\Games\\Fable\\escape-menu.log"

/* --- verified addresses (see docs/guild-seal.md, docs/ui-system.md) --- */
#define GSI_PTR           0x0143E8F8u  /* CGameScriptInterface *            */
#define FN_NAME_TO_DEFID  0x009AD410u  /* __thiscall(defmgr, CharString*)   */
#define FN_CHARSTR_CTOR   0x0099EBF0u
#define FN_CHARSTR_DTOR   0x0099EAE0u
#define FACTORY_TYPE_READ 0x0041D249u  /* mov eax,[ebx+0x3C] -- def in EBX  */

#define DEF_TYPE   0x3C                /* component type in a definition    */
#define DEF_ID     0x20                /* definition id (factory arg1)      */
#define DEF_X      0x58
#define DEF_Y      0x5C
#define DEF_KIDS   0x70                /* vector<uint32> begin/end/cap      */

#define TYPE_SCREEN_A 0x0A
#define TYPE_SCREEN_B 0x0C
#define TYPE_TEXT     0x06

/* The Escape menu. UI_TOP_LEVEL_MENU_SCREEN is the screen; the others are
 * tried too because which one is constructed depends on context. */
static const char *g_screen_names[] = {
    "UI_TOP_LEVEL_MENU_SCREEN",
    "UI_TOP_LEVEL_PAUSE_SCREEN",
    "UI_LIST_PAUSE_MENU",
    "UI_TOP_LEVEL_MENU",
};
#define N_SCREENS (int)(sizeof g_screen_names / sizeof g_screen_names[0])

#define LABEL "Crossover"

/* Pre-allocated replacement child vectors -- never allocate in the handler. */
#define POOL_SLOTS 32
#define POOL_ELEMS 64
static DWORD  g_pool[POOL_SLOTS][POOL_ELEMS];
static volatile LONG g_pool_next;

static FILE *g_log;
static DWORD g_screen_id[N_SCREENS];
static volatile DWORD g_entry_id;      /* def id of the component we append */
static volatile DWORD g_entry_def;     /* its definition, for the label     */
static volatile LONG  g_patched, g_hits;
static unsigned char  g_orig_byte;
static int            g_orig_saved;

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

static void charstr_ctor(void *cs, const char *s)
{
    __asm__ __volatile__("pushl $-1\n\tpushl %1\n\tmovl %0, %%ecx\n\tcall *%2\n\t"
        : : "r"(cs), "r"(s), "r"((void *)FN_CHARSTR_CTOR)
        : "eax", "ecx", "edx", "memory");
}
static void charstr_dtor(void *cs)
{
    __asm__ __volatile__("movl %0, %%ecx\n\tcall *%1\n\t"
        : : "r"(cs), "r"((void *)FN_CHARSTR_DTOR)
        : "eax", "ecx", "edx", "memory");
}
static DWORD thiscall1(DWORD fn, void *self, void *a)
{
    DWORD ret;
    __asm__ __volatile__("pushl %3\n\tmovl %1, %%ecx\n\tcall *%2\n\t"
        : "=a"(ret) : "r"(self), "r"(fn), "r"(a) : "ecx", "edx", "memory");
    return ret;
}

/* Resolve a definition name to its id, on the probe thread and once only --
 * never from inside the handler. */
static DWORD def_id_of(DWORD defmgr, const char *name)
{
    void *cs = NULL;
    DWORD id;
    charstr_ctor(&cs, name);
    id = thiscall1(FN_NAME_TO_DEFID, (void *)(uintptr_t)defmgr, &cs);
    charstr_dtor(&cs);
    return id;
}

static LONG CALLBACK bp_handler(EXCEPTION_POINTERS *ep)
{
    DWORD d, type, id;
    int i;

    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT ||
        (DWORD)(uintptr_t)ep->ExceptionRecord->ExceptionAddress != FACTORY_TYPE_READ)
        return EXCEPTION_CONTINUE_SEARCH;

    g_hits++;
    d = ep->ContextRecord->Ebx;

    if (!IsBadReadPtr((void *)(uintptr_t)(d + DEF_KIDS + 8), 4)) {
        type = *(DWORD *)(uintptr_t)(d + DEF_TYPE);
        id   = *(DWORD *)(uintptr_t)(d + DEF_ID);

        /* Adopt the first CText definition seen; it becomes the entry we append
         * and whose string is rewritten to "Crossover". */
        if (!g_entry_id && type == TYPE_TEXT) {
            g_entry_def = d;
            g_entry_id  = id;
            *(float *)(uintptr_t)(d + DEF_X) = 420.0f;
            *(float *)(uintptr_t)(d + DEF_Y) = 430.0f;
        }

        /* Append to the Escape menu screen's child list. */
        if (g_entry_id && (type == TYPE_SCREEN_A || type == TYPE_SCREEN_B)) {
            for (i = 0; i < N_SCREENS; i++) {
                if (!g_screen_id[i] || g_screen_id[i] != id) continue;
                {
                    DWORD b = *(DWORD *)(uintptr_t)(d + DEF_KIDS);
                    DWORD e = *(DWORD *)(uintptr_t)(d + DEF_KIDS + 4);
                    if (b > 0x10000 && e > b && (e - b) < POOL_ELEMS * 4 - 8) {
                        DWORD n = (e - b) / 4, k;
                        LONG slot = g_pool_next;
                        if (slot < POOL_SLOTS) {
                            DWORD *nv = g_pool[slot];
                            g_pool_next = slot + 1;
                            for (k = 0; k < n; k++)
                                nv[k] = *(DWORD *)(uintptr_t)(b + k * 4);
                            nv[n] = g_entry_id;
                            *(DWORD *)(uintptr_t)(d + DEF_KIDS)     = (DWORD)(uintptr_t)nv;
                            *(DWORD *)(uintptr_t)(d + DEF_KIDS + 4) = (DWORD)(uintptr_t)(nv + n + 1);
                            *(DWORD *)(uintptr_t)(d + DEF_KIDS + 8) = (DWORD)(uintptr_t)(nv + n + 1);
                            g_patched++;
                        }
                    }
                }
                break;
            }
        }
    }

    /* Restore and re-run the real instruction. The page was left writable at
     * arm time, so there is no Win32 call here. The probe thread re-arms. */
    *(unsigned char *)(uintptr_t)FACTORY_TYPE_READ = g_orig_byte;
    ep->ContextRecord->Eip = FACTORY_TYPE_READ;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static int arm(void)
{
    DWORD prot;
    if (!g_orig_saved) {
        if (!VirtualProtect((void *)(uintptr_t)FACTORY_TYPE_READ, 1,
                            PAGE_EXECUTE_READWRITE, &prot))
            return 0;
        g_orig_byte = *(unsigned char *)(uintptr_t)FACTORY_TYPE_READ;
        if (g_orig_byte == 0xCC) return 0;   /* never save 0xCC as "original" */
        g_orig_saved = 1;
    }
    *(unsigned char *)(uintptr_t)FACTORY_TYPE_READ = 0xCC;
    return 1;
}

static DWORD WINAPI probe_main(LPVOID unused)
{
    DWORD gsi = 0, defmgr = 0;
    int i, t;
    (void)unused;

    g_log = fopen(LOG_PATH, "w");
    if (!g_log) return 0;
    plog("=== escape-menu: add \"%s\" to the in-game menu ===", LABEL);

    for (t = 0; t < 600 && !defmgr; t++) {
        gsi = rd(GSI_PTR);
        if (gsi) defmgr = rd(gsi + 0x10);
        if (!defmgr) Sleep(500);
    }
    if (!defmgr) { plog("no definition manager; giving up"); goto done; }
    plog("definition manager 0x%08lX", defmgr);

    for (i = 0; i < N_SCREENS; i++) {
        g_screen_id[i] = def_id_of(defmgr, g_screen_names[i]);
        plog("  %-26s -> id %ld", g_screen_names[i], (long)g_screen_id[i]);
    }

    if (!AddVectoredExceptionHandler(1, bp_handler)) {
        plog("AddVectoredExceptionHandler failed");
        goto done;
    }
    if (!arm()) { plog("could not arm 0x%08X", FACTORY_TYPE_READ); goto done; }
    plog("armed at 0x%08X (original byte 0x%02X)", FACTORY_TYPE_READ, g_orig_byte);
    plog("open the Escape menu now");

    /* Re-arm tightly: components are built in a burst, and a slow cadence
     * misses most of them. Also keep the label rewritten, because the text is
     * baked to glyphs at construction. */
    for (t = 0; t < 120000; t++) {
        if (*(unsigned char *)(uintptr_t)FACTORY_TYPE_READ != 0xCC && g_orig_saved)
            *(unsigned char *)(uintptr_t)FACTORY_TYPE_READ = 0xCC;
        if (t % 2000 == 0)
            plog("  t=%lds  hits=%ld  screens patched=%ld  entry id=%ld",
                 (long)(t / 200), (long)g_hits, (long)g_patched, (long)g_entry_id);
        Sleep(5);
    }

done:
    plog("=== done: hits=%ld patched=%ld ===", (long)g_hits, (long)g_patched);
    if (g_log) { fclose(g_log); g_log = NULL; }
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
