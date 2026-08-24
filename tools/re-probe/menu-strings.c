/*
 * menu-strings: find a menu label's text in memory and work backwards to
 * whatever names it.
 *
 * Diffing definitions has taken this as far as it goes. The chain under a row
 * is known -- button (11) -> CChangingStateComponent (5) -> CText (6) -- and no
 * definition in it holds a string, so the label is referenced by an id. But
 * picking the id by shape has not paid: CText+0x38 was the only field distinct
 * across all six rows and sized like a table index, and writing Options' value
 * over Quit's produced no visible change.
 *
 * That leaves two possibilities and no way to tell them apart by staring at
 * more fields: either +0x38 is not the text, or the menu was never rebuilt so
 * the definition was never re-read. This run settles both.
 *
 *   1. The +0x38 write now happens BEFORE the menu is constructed. The
 *      definitions are loaded long before the frontend is built, so a scan
 *      that starts at process start finds Quit's CText and writes it in good
 *      time. Whatever the first menu shows is then the honest answer.
 *
 *   2. Independently, it hunts the string itself. "Credits" and "Options" must
 *      exist as characters somewhere for the game to draw them. Find those
 *      bytes, then find every pointer to them, and the field that names a
 *      label stops being a guess -- it is whatever is holding that pointer.
 *      Both ASCII and UTF-16 are searched, because a localised game may store
 *      either and the earlier probes only ever looked for ASCII.
 *
 * Read-only apart from the single 4-byte +0x38 write.
 */

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#define LOG_PATH "menu-strings.log"

#define CUIDEF_VTABLE 0x01259F8Cu
#define DEF_ID   0x20
#define DEF_TEXT 0x38
#define DEF_TYPE 0x3C

#define QUIT_TEXT_DEF    254
#define OPTIONS_TEXT_DEF 269

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

/* --- generic memory walk --- */

typedef void (*RegionFn)(unsigned char *base, SIZE_T size, void *ctx);

static void walk_memory(RegionFn fn, void *ctx)
{
    MEMORY_BASIC_INFORMATION mbi;
    unsigned char *addr = (unsigned char *)0x00010000;
    unsigned char *limit = (unsigned char *)0x7FFF0000;

    while (addr < limit && VirtualQuery(addr, &mbi, sizeof mbi) == sizeof mbi) {
        unsigned char *base = (unsigned char *)mbi.BaseAddress;
        SIZE_T size = mbi.RegionSize;
        if (readable(&mbi) && size >= 0x400) fn(base, size, ctx);
        addr = base + size;
        if (size == 0) break;
    }
}

/* --- find a definition by id --- */

typedef struct { DWORD id, def; } FindCtx;

static void find_def_cb(unsigned char *base, SIZE_T size, void *ctx)
{
    FindCtx *f = (FindCtx *)ctx;
    SIZE_T off;
    if (f->def) return;
    for (off = 0; off + 0x200 <= size; off += 4) {
        DWORD *p = (DWORD *)(base + off);
        if (p[0] == CUIDEF_VTABLE && p[DEF_ID / 4] == f->id) {
            f->def = (DWORD)(uintptr_t)p;
            return;
        }
    }
}

static DWORD find_def(DWORD id)
{
    FindCtx f;
    f.id = id; f.def = 0;
    walk_memory(find_def_cb, &f);
    return f.def;
}

/* --- hunt a string, then hunt pointers to it --- */

#define MAX_HITS 32
typedef struct {
    const char *needle;
    int wide;
    DWORD hits[MAX_HITS];
    int nhits;
} StrCtx;

static void find_str_cb(unsigned char *base, SIZE_T size, void *ctx)
{
    StrCtx *s = (StrCtx *)ctx;
    int n = (int)strlen(s->needle);
    SIZE_T off;

    for (off = 0; off + (SIZE_T)(n * (s->wide ? 2 : 1)) + 2 <= size; off++) {
        int i, ok = 1;
        for (i = 0; i < n && ok; i++) {
            unsigned char c = base[off + (s->wide ? i * 2 : i)];
            if (c != (unsigned char)s->needle[i]) ok = 0;
            if (ok && s->wide && base[off + i * 2 + 1] != 0) ok = 0;
        }
        if (!ok) continue;
        /* must terminate, or it is a substring of something longer */
        {
            unsigned char t = base[off + (s->wide ? n * 2 : n)];
            if (t != 0) continue;
        }
        if (s->nhits < MAX_HITS) s->hits[s->nhits++] = (DWORD)(uintptr_t)(base + off);
        if (s->nhits >= MAX_HITS) return;
    }
}

typedef struct {
    const DWORD *targets;
    int ntargets;
    DWORD holders[MAX_HITS];
    DWORD points_to[MAX_HITS];
    int n;
} PtrCtx;

static void find_ptr_cb(unsigned char *base, SIZE_T size, void *ctx)
{
    PtrCtx *p = (PtrCtx *)ctx;
    SIZE_T off;
    for (off = 0; off + 4 <= size; off += 4) {
        DWORD v = *(DWORD *)(base + off);
        int i;
        for (i = 0; i < p->ntargets; i++) {
            if (v != p->targets[i]) continue;
            if (p->n < MAX_HITS) {
                p->holders[p->n] = (DWORD)(uintptr_t)(base + off);
                p->points_to[p->n] = v;
                p->n++;
            }
            break;
        }
        if (p->n >= MAX_HITS) return;
    }
}

/* Is `addr` inside a CUIDef? If so, say which and at what offset. */
typedef struct { DWORD addr, def, id, type; int off; } OwnerCtx;

static void find_owner_cb(unsigned char *base, SIZE_T size, void *ctx)
{
    OwnerCtx *o = (OwnerCtx *)ctx;
    SIZE_T off;
    if (o->def) return;
    for (off = 0; off + 0x200 <= size; off += 4) {
        DWORD *p = (DWORD *)(base + off);
        DWORD start = (DWORD)(uintptr_t)p;
        if (p[0] != CUIDEF_VTABLE) continue;
        if (o->addr >= start && o->addr < start + 0x200) {
            o->def  = start;
            o->id   = p[DEF_ID / 4];
            o->type = p[DEF_TYPE / 4];
            o->off  = (int)(o->addr - start);
            return;
        }
    }
}

static void hunt(const char *needle, int wide)
{
    StrCtx s;
    PtrCtx p;
    int i;

    memset(&s, 0, sizeof s);
    s.needle = needle; s.wide = wide;
    walk_memory(find_str_cb, &s);

    plog("");
    plog("--- \"%s\" (%s): %d occurrence(s) ---", needle,
         wide ? "UTF-16" : "ASCII", s.nhits);
    if (!s.nhits) return;

    for (i = 0; i < s.nhits && i < 8; i++)
        plog("    at 0x%08lX", s.hits[i]);

    memset(&p, 0, sizeof p);
    p.targets = s.hits;
    p.ntargets = s.nhits < 8 ? s.nhits : 8;
    walk_memory(find_ptr_cb, &p);

    plog("  pointers to it: %d", p.n);
    for (i = 0; i < p.n && i < 12; i++) {
        OwnerCtx o;
        memset(&o, 0, sizeof o);
        o.addr = p.holders[i];
        walk_memory(find_owner_cb, &o);
        if (o.def)
            plog("    0x%08lX -> 0x%08lX   inside CUIDef id %lu type %lu at +0x%X",
                 p.holders[i], p.points_to[i], o.id, o.type, o.off);
        else
            plog("    0x%08lX -> 0x%08lX", p.holders[i], p.points_to[i]);
    }
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
    DWORD quit = 0, options = 0, want = 0;
    int t;

    (void)unused;
    g_log = fopen(LOG_PATH, "w");
    plog("=== menu-strings: find the label text, and who names it ===");

    /* Part 1 -- get the +0x38 write in before the menu is ever constructed. */
    for (t = 0; t < 600 && !quit; t++) {
        quit = find_def(QUIT_TEXT_DEF);
        if (!quit) { dismiss_dialogs(); Sleep(500); }
    }
    options = quit ? find_def(OPTIONS_TEXT_DEF) : 0;

    if (quit && options) {
        want = *(DWORD *)(uintptr_t)(options + DEF_TEXT);
        plog("Quit's CText def 0x%08lX  +0x38 = %lu", quit,
             *(DWORD *)(uintptr_t)(quit + DEF_TEXT));
        plog("Options' CText def 0x%08lX  +0x38 = %lu", options, want);
        *(DWORD *)(uintptr_t)(quit + DEF_TEXT) = want;
        plog("written BEFORE the menu is built -- the first menu shown is the "
             "answer");
    } else {
        plog("definitions not found (quit=0x%08lX options=0x%08lX)", quit, options);
    }

    /* Part 2 -- let the frontend come up, then hunt the strings. */
    for (t = 0; t < 400; t++) {
        if (quit && want && *(DWORD *)(uintptr_t)(quit + DEF_TEXT) != want)
            *(DWORD *)(uintptr_t)(quit + DEF_TEXT) = want;
        dismiss_dialogs();
        Sleep(100);
    }

    plog("");
    plog("=== hunting the label strings ===");
    hunt("Credits", 0);
    hunt("Credits", 1);
    hunt("Change Profile", 0);
    hunt("Change Profile", 1);

    plog("");
    plog("=== done ===");

    for (t = 0; t < 30000; t++) {
        if (quit && want && *(DWORD *)(uintptr_t)(quit + DEF_TEXT) != want)
            *(DWORD *)(uintptr_t)(quit + DEF_TEXT) = want;
        Sleep(200);
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
