/*
 * menu-utf16: read a menu label's actual text, and find the field that names it.
 *
 * Correcting a mistake. Every earlier probe concluded "no string is reachable
 * from any of these definitions" -- and that conclusion was an artefact of the
 * reader, not a fact about the game. read_text() walked bytes and stopped at
 * the first NUL, so a UTF-16 label ('C', 0, 'r', 0, ...) looked like a
 * one-character string and was rejected as noise. The game is localised; of
 * course its text is wide.
 *
 * The tell came from hunting the bytes directly: "Credits" exists exactly once
 * as UTF-16, at an address in the same heap as the definitions, with a pointer
 * to it sitting 0x20 bytes in front of it -- a string object with a pointer
 * field and an inline buffer.
 *
 * So this does three things, all read-only:
 *
 *   1. Reads every field of the six rows' CText definitions (and their parents
 *      and state records) as a possible pointer to WIDE text, one and two hops
 *      out. If a definition names its label, this prints it.
 *
 *   2. Walks back from the text instead of forward from the definition: find
 *      the characters, find what points at them, then find what points at
 *      that, and report whether the holder lands inside a CUIDef and at which
 *      offset. Whatever that offset is, it is the field -- no shape-guessing.
 *
 *   3. Ignores hits inside this DLL. The previous run matched its own needle
 *      constants and reported them as findings, which is how "Credits" appeared
 *      to live at two ASCII addresses.
 */

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#define LOG_PATH "menu-utf16.log"

#define CUIDEF_VTABLE 0x01259F8Cu
#define DEF_ID   0x20
#define DEF_TYPE 0x3C
#define DEF_STATES     0x40
#define DEF_STATES_END 0x44
#define STATE_SIZE 0x7C

static const char *ROW_NAME[] = {
    "Continue Game", "Change Profile", "Options", "Credits", "About", "Quit"
};
static const DWORD ROW_TEXTDEF[] = { 265, 250, 269, 285, 257, 254 };
static const DWORD ROW_MIDDEF[]  = { 263, 248, 267, 283, 256, 252 };
static const DWORD ROW_BUTTON[]  = { 262, 247, 266, 280, 255, 251 };
#define ROWS 6

static FILE *g_log;
static DWORD g_self_lo, g_self_hi;      /* this DLL, to exclude from hits */

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

typedef void (*RegionFn)(unsigned char *base, SIZE_T size, void *ctx);

static void walk_memory(RegionFn fn, void *ctx)
{
    MEMORY_BASIC_INFORMATION mbi;
    unsigned char *addr = (unsigned char *)0x00010000;
    unsigned char *limit = (unsigned char *)0x7FFF0000;

    while (addr < limit && VirtualQuery(addr, &mbi, sizeof mbi) == sizeof mbi) {
        unsigned char *base = (unsigned char *)mbi.BaseAddress;
        SIZE_T size = mbi.RegionSize;
        if (readable(&mbi) && size >= 0x400 &&
            !IS_SELF((DWORD)(uintptr_t)base))
            fn(base, size, ctx);
        addr = base + size;
        if (size == 0) break;
    }
}

/* --- wide text --- */

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
    return i >= 3;
}

static int read_ascii(DWORD p, char *out, int cap)
{
    int i;
    if (!PLAUSIBLE(p) || IsBadReadPtr((void *)(uintptr_t)p, 1)) return 0;
    for (i = 0; i < cap - 1; i++) {
        unsigned char c;
        if (IsBadReadPtr((void *)(uintptr_t)(p + i), 1)) break;
        c = *(unsigned char *)(uintptr_t)(p + i);
        if (c == 0) break;
        if (c < 0x20 || c > 0x7E) return 0;
        out[i] = (char)c;
    }
    out[i] = 0;
    return i >= 3;
}

/* Report any text reachable from value v, up to two pointer hops. */
static int text_from(DWORD v, char *out, int cap, const char **how)
{
    DWORD inner;

    if (read_wide(v, out, cap))  { *how = "W";    return 1; }
    if (read_ascii(v, out, cap)) { *how = "A";    return 1; }
    if (!PLAUSIBLE(v) || IsBadReadPtr((void *)(uintptr_t)v, 4)) return 0;
    inner = *(DWORD *)(uintptr_t)v;
    if (read_wide(inner, out, cap))  { *how = "->W"; return 1; }
    if (read_ascii(inner, out, cap)) { *how = "->A"; return 1; }
    return 0;
}

static DWORD at(DWORD o, int off) { return *(DWORD *)(uintptr_t)(o + off); }

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
    FindCtx f; f.id = id; f.def = 0;
    walk_memory(find_def_cb, &f);
    return f.def;
}

/* --- 1: forward, every field of a definition --- */

static void scan_object(const char *what, DWORD obj, int size)
{
    int off, found = 0;

    if (!obj) return;
    for (off = 0; off + 4 <= size; off += 4) {
        char text[128];
        const char *how;
        if (text_from(at(obj, off), text, sizeof text, &how)) {
            plog("      +0x%03X %-3s \"%s\"", off, how, text);
            found++;
        }
    }
    if (!found) plog("      (%s: no text reachable)", what);
}

/* --- 2: backward, from the characters to the field --- */

typedef struct {
    const char *needle;
    DWORD hits[8];
    int n;
} WideCtx;

static void find_wide_cb(unsigned char *base, SIZE_T size, void *ctx)
{
    WideCtx *w = (WideCtx *)ctx;
    int n = (int)strlen(w->needle);
    SIZE_T off;

    for (off = 0; off + (SIZE_T)(n * 2) + 2 <= size; off += 2) {
        int i, ok = 1;
        for (i = 0; i < n && ok; i++) {
            if (base[off + i * 2] != (unsigned char)w->needle[i] ||
                base[off + i * 2 + 1] != 0) ok = 0;
        }
        if (!ok) continue;
        if (*(unsigned short *)(base + off + n * 2) != 0) continue;
        if (w->n < 8) w->hits[w->n++] = (DWORD)(uintptr_t)(base + off);
        if (w->n >= 8) return;
    }
}

typedef struct {
    DWORD lo, hi;                 /* accept pointers landing in [lo,hi) */
    DWORD holders[64];
    DWORD vals[64];
    int n;
} RefCtx;

static void find_refs_cb(unsigned char *base, SIZE_T size, void *ctx)
{
    RefCtx *r = (RefCtx *)ctx;
    SIZE_T off;
    for (off = 0; off + 4 <= size; off += 4) {
        DWORD v = *(DWORD *)(base + off);
        if (v < r->lo || v >= r->hi) continue;
        if (r->n < 64) {
            r->holders[r->n] = (DWORD)(uintptr_t)(base + off);
            r->vals[r->n] = v;
            r->n++;
        } else return;
    }
}

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
            o->def = start; o->id = p[DEF_ID / 4]; o->type = p[DEF_TYPE / 4];
            o->off = (int)(o->addr - start);
            return;
        }
    }
}

static void backtrack(const char *needle)
{
    WideCtx w;
    RefCtx r1, r2;
    int i, j;

    memset(&w, 0, sizeof w);
    w.needle = needle;
    walk_memory(find_wide_cb, &w);

    plog("");
    plog("--- \"%s\" as UTF-16: %d occurrence(s) ---", needle, w.n);
    for (i = 0; i < w.n; i++) plog("    characters at 0x%08lX", w.hits[i]);
    if (!w.n) return;

    /* Who points at the characters, or just in front of them (a string object
     * keeps its buffer a little way past its own base). */
    memset(&r1, 0, sizeof r1);
    r1.lo = w.hits[0];
    r1.hi = w.hits[0] + 4;
    walk_memory(find_refs_cb, &r1);
    plog("  %d pointer(s) to the characters:", r1.n);

    for (i = 0; i < r1.n && i < 6; i++) {
        OwnerCtx o;
        memset(&o, 0, sizeof o);
        o.addr = r1.holders[i];
        walk_memory(find_owner_cb, &o);
        if (o.def)
            plog("    holder 0x%08lX  IS INSIDE CUIDef id %lu type %lu at +0x%X",
                 r1.holders[i], o.id, o.type, o.off);
        else
            plog("    holder 0x%08lX  (not a CUIDef)", r1.holders[i]);

        /* And who points at that holder's object? */
        memset(&r2, 0, sizeof r2);
        r2.lo = r1.holders[i] > 0x40 ? r1.holders[i] - 0x40 : 0;
        r2.hi = r1.holders[i] + 4;
        walk_memory(find_refs_cb, &r2);
        for (j = 0; j < r2.n && j < 6; j++) {
            OwnerCtx o2;
            memset(&o2, 0, sizeof o2);
            o2.addr = r2.holders[j];
            walk_memory(find_owner_cb, &o2);
            if (o2.def)
                plog("        <- 0x%08lX  INSIDE CUIDef id %lu type %lu at +0x%X",
                     r2.holders[j], o2.id, o2.type, o2.off);
        }
    }
}

static DWORD WINAPI worker(LPVOID unused)
{
    MEMORY_BASIC_INFORMATION mbi;
    int k;

    (void)unused;
    g_log = fopen(LOG_PATH, "w");

    /* Exclude ourselves: last run matched its own needle constants. */
    if (VirtualQuery((void *)worker, &mbi, sizeof mbi) == sizeof mbi) {
        g_self_lo = (DWORD)(uintptr_t)mbi.AllocationBase;
        g_self_hi = g_self_lo + 0x200000;
    }
    plog("=== menu-utf16: the label text, read as wide ===");
    plog("this DLL is 0x%08lX..0x%08lX and is excluded", g_self_lo, g_self_hi);

    plog("");
    plog("=== text reachable from each row's definitions (W=wide, A=ascii) ===");
    for (k = 0; k < ROWS; k++) {
        DWORD b = find_def(ROW_BUTTON[k]);
        DWORD m = find_def(ROW_MIDDEF[k]);
        DWORD t = find_def(ROW_TEXTDEF[k]);
        DWORD sb, se;

        plog("  %s:", ROW_NAME[k]);
        plog("    button id %lu def 0x%08lX", ROW_BUTTON[k], b);
        scan_object("button", b, 0x200);
        plog("    middle id %lu def 0x%08lX", ROW_MIDDEF[k], m);
        scan_object("middle", m, 0x200);
        plog("    CText  id %lu def 0x%08lX", ROW_TEXTDEF[k], t);
        scan_object("CText", t, 0x200);

        if (t) {
            sb = at(t, DEF_STATES);
            se = at(t, DEF_STATES_END);
            if (PLAUSIBLE(sb) && se > sb) {
                DWORD n = (se - sb) / STATE_SIZE, s;
                for (s = 0; s < n && s < 8; s++) {
                    plog("    CText state[%lu]:", s);
                    scan_object("state", sb + s * STATE_SIZE, STATE_SIZE);
                }
            }
        }
    }

    plog("");
    plog("=== backwards, from the characters to the field ===");
    backtrack("Credits");
    backtrack("Change Profile");

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
