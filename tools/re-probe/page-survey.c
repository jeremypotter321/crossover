/*
 * page-survey: dump every UI definition the game has loaded, decoded.
 *
 * Everything needed to put our own row in the main menu is proved (sections
 * 13-16, plus the action at CFrontEndButton +0xC4 confirmed by clicking).
 * A page of our own is the next thing, and it needs a map rather than another
 * single experiment: which screen an action opens, what a screen contains, and
 * which button returns to the menu.
 *
 * So this reads the whole frontend at once. Definitions are found by scanning
 * for the CUIDef vtable 0x01259F8C -- no breakpoint, no construction burst to
 * race -- and each is printed with the three fields that now have meaning:
 *
 *     type      +0x3C   the factory's jump-table index
 *     action    +0xC4   for a button (11): what clicking it does
 *     key       +0x54   for a CText (6): its wide text key, drawn as-is when
 *                       the language bank has no entry for it
 *     children  +0x70   the ids this definition builds
 *
 * Attach while sitting on the screen of interest: a definition is only in
 * memory once something has loaded it.
 *
 * Read-only.
 */

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

/* Absolute, deliberately. A probe writes relative to the process working
 * directory, and that is not always the game folder: launched by the mod
 * loader, Fable.exe runs with FableModLoader/ as its cwd, and a relative path
 * then lands somewhere unwritable and the log never appears at all. */
#define LOG_PATH "C:\\Games\\Fable\\page-survey.log"

#define CUIDEF_VTABLE 0x01259F8Cu

#define DEF_ID          0x20
#define DEF_TYPE        0x3C
#define DEF_KEY         0x54
#define DEF_ACTION      0xC4
#define DEF_CHILD_BEGIN 0x70
#define DEF_CHILD_END   0x74

#define TYPE_TEXT   6
#define TYPE_BUTTON 11
#define TYPE_LIST   12
#define TYPE_SCREEN 10

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

#define PLAUSIBLE(p) ((p) >= 0x00400000u && (p) < 0x20000000u)
#define IS_SELF(p)   ((p) >= g_self_lo && (p) < g_self_hi)

static int readable(const MEMORY_BASIC_INFORMATION *mbi)
{
    DWORD p = mbi->Protect;
    if (mbi->State != MEM_COMMIT) return 0;
    if (p & (PAGE_GUARD | PAGE_NOACCESS)) return 0;
    return (p & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                 PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                 PAGE_EXECUTE_WRITECOPY)) != 0;
}

#define MAX_DEFS 2048
typedef struct {
    DWORD def, id, type, action;
    DWORD kids[24];
    DWORD nkids;
    char key[80];
} Def;

static Def g_defs[MAX_DEFS];
static int g_n;

static int read_wide(DWORD p, char *out, int cap)
{
    int i;
    if (!PLAUSIBLE(p) || IsBadReadPtr((void *)(uintptr_t)p, 2)) return 0;
    for (i = 0; i < cap - 1; i++) {
        unsigned short w;
        if (IsBadReadPtr((void *)(uintptr_t)(p + i * 2), 2)) break;
        w = *(unsigned short *)(uintptr_t)(p + i * 2);
        if (w == 0) break;
        if (w < 0x20 || w > 0x7E) return 0;
        out[i] = (char)w;
    }
    out[i] = 0;
    return i >= 2;
}

static void collect_cb(unsigned char *base, SIZE_T size, void *ctx)
{
    SIZE_T off;
    (void)ctx;
    for (off = 0; off + 0x200 <= size; off += 4) {
        DWORD *p = (DWORD *)(base + off);
        Def *d;
        DWORD b, e;

        if (p[0] != CUIDEF_VTABLE) continue;
        if (g_n >= MAX_DEFS) return;

        d = &g_defs[g_n++];
        memset(d, 0, sizeof *d);
        d->def    = (DWORD)(uintptr_t)p;
        d->id     = p[DEF_ID / 4];
        d->type   = p[DEF_TYPE / 4];
        d->action = p[DEF_ACTION / 4];

        if (d->type == TYPE_TEXT) {
            DWORD strobj = p[DEF_KEY / 4];
            if (PLAUSIBLE(strobj) && !IsBadReadPtr((void *)(uintptr_t)strobj, 4))
                read_wide(*(DWORD *)(uintptr_t)strobj, d->key, sizeof d->key);
        }

        b = p[DEF_CHILD_BEGIN / 4];
        e = p[DEF_CHILD_END / 4];
        if (PLAUSIBLE(b) && e > b && (e - b) <= 24 * 4) {
            DWORD n = (e - b) / 4, k;
            d->nkids = n;
            for (k = 0; k < n; k++)
                d->kids[k] = *(DWORD *)(uintptr_t)(b + k * 4);
        }
    }
}

static void walk_memory(void)
{
    MEMORY_BASIC_INFORMATION mbi;
    unsigned char *addr = (unsigned char *)0x00010000;
    unsigned char *limit = (unsigned char *)0x7FFF0000;

    while (addr < limit && VirtualQuery(addr, &mbi, sizeof mbi) == sizeof mbi) {
        unsigned char *base = (unsigned char *)mbi.BaseAddress;
        SIZE_T size = mbi.RegionSize;
        if (readable(&mbi) && size >= 0x400 && !IS_SELF((DWORD)(uintptr_t)base))
            collect_cb(base, size, NULL);
        addr = base + size;
        if (size == 0) break;
    }
}

static const Def *by_id(DWORD id)
{
    int i;
    for (i = 0; i < g_n; i++) if (g_defs[i].id == id) return &g_defs[i];
    return NULL;
}

static const char *type_name(DWORD t)
{
    switch (t) {
    case 0:  return "sprite";
    case 2:  return "cont2";
    case 5:  return "changing";
    case 6:  return "TEXT";
    case 10: return "SCREEN";
    case 11: return "BUTTON";
    case 12: return "LIST";
    case 22: return "container";
    default: return "?";
    }
}

static void show(const Def *d, int depth, int *seen, int nseen)
{
    char pad[32];
    int i;
    DWORD k;

    if (!d || depth > 4) return;
    for (i = 0; i < depth && i < 15; i++) { pad[i*2] = ' '; pad[i*2+1] = ' '; }
    pad[depth * 2] = 0;

    plog("  %s%-4lu %-9s%s%s%s%s", pad, d->id, type_name(d->type),
         d->type == TYPE_BUTTON ? "  action=" : "",
         d->type == TYPE_BUTTON ? "" : "",
         d->type == TYPE_TEXT && d->key[0] ? "  \"" : "",
         d->type == TYPE_TEXT && d->key[0] ? d->key : "");
    if (d->type == TYPE_BUTTON)
        plog("  %s      action %lu", pad, d->action);

    for (i = 0; i < nseen; i++) if (seen[i] == (int)d->id) return;
    if (nseen < 64) { seen[nseen] = (int)d->id; nseen++; }

    for (k = 0; k < d->nkids; k++)
        show(by_id(d->kids[k]), depth + 1, seen, nseen);
}

static DWORD WINAPI worker(LPVOID unused)
{
    MEMORY_BASIC_INFORMATION mbi;
    int i;

    (void)unused;
    g_log = fopen(LOG_PATH, "w");
    if (VirtualQuery((void *)worker, &mbi, sizeof mbi) == sizeof mbi) {
        g_self_lo = (DWORD)(uintptr_t)mbi.AllocationBase;
        g_self_hi = g_self_lo + 0x200000;
    }
    plog("=== page-survey: every loaded UI definition ===");

    walk_memory();
    plog("%d definition(s)", g_n);

    plog("");
    plog("=== screens, expanded ===");
    for (i = 0; i < g_n; i++) {
        int seen[64];
        if (g_defs[i].type != TYPE_SCREEN) continue;
        plog("");
        plog("SCREEN %lu (def 0x%08lX, %lu children):",
             g_defs[i].id, g_defs[i].def, g_defs[i].nkids);
        show(&g_defs[i], 1, seen, 0);
    }

    plog("");
    plog("=== every button: id, action, and its label key ===");
    for (i = 0; i < g_n; i++) {
        const Def *b = &g_defs[i], *mid, *txt;
        DWORD k;
        const char *label = "";

        if (b->type != TYPE_BUTTON) continue;
        mid = b->nkids ? by_id(b->kids[0]) : NULL;
        if (mid)
            for (k = 0; k < mid->nkids; k++) {
                txt = by_id(mid->kids[k]);
                if (txt && txt->type == TYPE_TEXT && txt->key[0]) {
                    label = txt->key; break;
                }
            }
        plog("  button %-5lu action %-6lu  %s", b->id, b->action, label);
    }

    plog("");
    plog("=== done ===");
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
