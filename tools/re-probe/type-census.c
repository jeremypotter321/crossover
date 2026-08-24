/*
 * type-census: what widget kinds are loaded, and which definition is which.
 *
 * The page is editable (page-mods), so the next question is what can be PUT on
 * it. A screen's contents are a list of child definition ids, and the factory
 * dispatches on the type at CUIDef +0x3C -- so the gallery we can build is
 * bounded by which definitions of which types are currently in memory.
 *
 * Definitions load per screen, not all at startup: a scan on the main menu
 * found ~618 and the same scan on the Options page found ~762. So this reports
 * what is loaded right now, grouped by type, with each CText's key so a widget
 * can be recognised by the label next to it.
 *
 * Read-only.
 */

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#define LOG_PATH "C:\\Games\\Fable\\type-census.log"

#define CUIDEF_VTABLE 0x01259F8Cu
#define DEF_ID          0x20
#define DEF_TYPE        0x3C
#define DEF_KEY         0x54
#define DEF_ACTION      0xC4
#define DEF_CHILD_BEGIN 0x70
#define DEF_CHILD_END   0x74

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

#define MAX_DEFS 3000
typedef struct {
    DWORD id, type, action, nkids, kids[24];
    char key[72];
} Def;
static Def g_d[MAX_DEFS];
static int g_n;

static int read_wide(DWORD p, char *out, int cap)
{
    int i;
    if (!PLAUSIBLE(p) || IsBadReadPtr((void *)(uintptr_t)p, 2)) return 0;
    for (i = 0; i < cap - 1; i++) {
        unsigned short w;
        if (IsBadReadPtr((void *)(uintptr_t)(p + i * 2), 2)) break;
        w = *(unsigned short *)(uintptr_t)(p + i * 2);
        if (!w) break;
        if (w < 0x20 || w > 0x7E) return 0;
        out[i] = (char)w;
    }
    out[i] = 0;
    return i >= 2;
}

static const char *tname(DWORD t)
{
    switch (t) {
    case 0:  return "CSprite";
    case 1:  return "CMorphingSprite";
    case 2:  return "container2";
    case 5:  return "CChangingStateComponent";
    case 6:  return "CText";
    case 10: return "CFrontEndScreen";
    case 11: return "CFrontEndButton";
    case 12: return "CFrontEndList";
    case 16: return "CTextSlider";
    case 22: return "CComponentContainer";
    case 25: return "CEditBox";
    case 38: return "CNavButton";
    default: return "(unnamed type)";
    }
}

static DWORD WINAPI worker(LPVOID unused)
{
    MEMORY_BASIC_INFORMATION mbi;
    unsigned char *addr = (unsigned char *)0x00010000;
    unsigned char *limit = (unsigned char *)0x7FFF0000;
    DWORD t;
    int i;

    (void)unused;
    g_log = fopen(LOG_PATH, "w");
    if (VirtualQuery((void *)worker, &mbi, sizeof mbi) == sizeof mbi) {
        g_self_lo = (DWORD)(uintptr_t)mbi.AllocationBase;
        g_self_hi = g_self_lo + 0x200000;
    }
    plog("=== type-census: widget kinds currently loaded ===");

    while (addr < limit && VirtualQuery(addr, &mbi, sizeof mbi) == sizeof mbi) {
        unsigned char *base = (unsigned char *)mbi.BaseAddress;
        SIZE_T size = mbi.RegionSize, off;
        DWORD pr = mbi.Protect;

        if (mbi.State == MEM_COMMIT && !(pr & (PAGE_GUARD | PAGE_NOACCESS)) &&
            (pr & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                   PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                   PAGE_EXECUTE_WRITECOPY)) &&
            size >= 0x400 && !IS_SELF((DWORD)(uintptr_t)base)) {
            for (off = 0; off + 0x200 <= size; off += 4) {
                DWORD *p = (DWORD *)(base + off);
                Def *d;
                DWORD b, e;
                if (p[0] != CUIDEF_VTABLE) continue;
                if (g_n >= MAX_DEFS) break;
                d = &g_d[g_n++];
                memset(d, 0, sizeof *d);
                d->id = p[DEF_ID / 4];
                d->type = p[DEF_TYPE / 4];
                d->action = p[DEF_ACTION / 4];
                if (d->type == 6) {
                    DWORD s = p[DEF_KEY / 4];
                    if (PLAUSIBLE(s) && !IsBadReadPtr((void *)(uintptr_t)s, 4))
                        read_wide(*(DWORD *)(uintptr_t)s, d->key, sizeof d->key);
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
        addr = base + size;
        if (size == 0) break;
    }

    plog("%d definition(s) loaded", g_n);
    plog("");
    plog("=== counts by type ===");
    for (t = 0; t <= 0x2B; t++) {
        int c = 0;
        for (i = 0; i < g_n; i++) if (g_d[i].type == t) c++;
        if (c) plog("  type %-3lu %-26s %d", t, tname(t), c);
    }

    /* The interesting ones, listed individually: anything that is not scenery. */
    plog("");
    plog("=== candidates for a gallery ===");
    for (t = 0; t <= 0x2B; t++) {
        int shown = 0;
        if (t == 0 || t == 2 || t == 5 || t == 6) continue;   /* scenery/text */
        for (i = 0; i < g_n && shown < 40; i++) {
            Def *d = &g_d[i];
            if (d->type != t) continue;
            {
                char kids[160];
                int off = 0;
                DWORD q;
                kids[0] = 0;
                for (q = 0; q < d->nkids && off < (int)sizeof kids - 8; q++)
                    off += snprintf(kids + off, sizeof kids - off, "%lu ",
                                    d->kids[q]);
                plog("  type %-3lu %-22s id %-5lu action %-6lu [%s]",
                     t, tname(t), d->id, d->action, kids);
            }
            shown++;
        }
    }

    plog("");
    plog("=== every CText key loaded (what labels exist) ===");
    for (i = 0; i < g_n; i++)
        if (g_d[i].type == 6 && g_d[i].key[0])
            plog("  CText %-5lu \"%s\"", g_d[i].id, g_d[i].key);

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
