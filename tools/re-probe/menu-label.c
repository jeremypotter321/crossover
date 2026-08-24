/*
 * menu-label: find what selects a menu row's text, by reading the definitions
 * the rows' labels actually come from.
 *
 * Two things are now known and they point at the same place.
 *
 * A CFrontEndButton definition holds no text -- menu-inspect found not one
 * string reachable from any of the six, and writing Options' value over Quit's
 * at the only mirrored per-button scalar (+0xC4/+0xE4) changed nothing on
 * screen. But every menu button has exactly one child definition, with an id
 * one or two above its own:
 *
 *     Continue 262 -> 263      Credits 280 -> 283
 *     Change   247 -> 248      About   255 -> 256
 *     Options  266 -> 267      Quit    251 -> 252
 *
 * and a button captured elsewhere (452) has a CText child (455). So the label
 * is a CText child of the button, and the field that selects its string is in
 * the child, not the button.
 *
 * Those six children were never captured, because a child is constructed after
 * its parent and the previous probe disarmed the moment it had all six parents.
 * Rather than replay the intro to widen that window -- which costs a launch and
 * re-rolls Wine's video-error dialog every time -- this finds the definitions
 * where they already are.
 *
 * Definitions are identifiable without the factory and without calling the
 * game: every CUIDef begins with vtable 0x01259F8C, and carries its id at
 * +0x20. So one pass over committed memory finds every definition currently
 * loaded, whenever we happen to attach, and nothing has to be timed.
 *
 * Read-only. Nothing is written and nothing in the game is called.
 */

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#define LOG_PATH "menu-label.log"

#define CUIDEF_VTABLE 0x01259F8Cu

#define DEF_ID          0x20
#define DEF_TYPE        0x3C
#define DEF_STATES      0x40
#define DEF_STATES_END  0x44
#define DEF_CHILD_BEGIN 0x70
#define DEF_CHILD_END   0x74

#define STATE_SIZE 0x7C
#define DEF_DUMP   0x200

/* The six menu rows, and the child definition each one's label lives in. */
static const char *ROW_NAME[] = {
    "Continue Game", "Change Profile", "Options", "Credits", "About", "Quit"
};
static const DWORD ROW_BUTTON[] = { 262, 247, 266, 280, 255, 251 };
static const DWORD ROW_LABEL[]  = { 263, 248, 267, 283, 256, 252 };
#define ROWS ((int)(sizeof ROW_BUTTON / sizeof ROW_BUTTON[0]))

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

/* --- the scan --- */

#define MAX_FOUND 4096
typedef struct { DWORD def, id, type; } Found;
static Found g_found[MAX_FOUND];
static int g_found_n;

static int readable(const MEMORY_BASIC_INFORMATION *mbi)
{
    DWORD p = mbi->Protect;

    if (mbi->State != MEM_COMMIT) return 0;
    if (p & PAGE_GUARD) return 0;
    if (p & PAGE_NOACCESS) return 0;
    return (p & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                 PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                 PAGE_EXECUTE_WRITECOPY)) != 0;
}

static void scan(void)
{
    MEMORY_BASIC_INFORMATION mbi;
    unsigned char *addr = (unsigned char *)0x00010000;
    unsigned char *limit = (unsigned char *)0x7FFF0000;
    DWORD regions = 0, bytes = 0;

    while (addr < limit && VirtualQuery(addr, &mbi, sizeof mbi) == sizeof mbi) {
        unsigned char *base = (unsigned char *)mbi.BaseAddress;
        SIZE_T size = mbi.RegionSize;

        if (readable(&mbi) && size >= DEF_DUMP) {
            SIZE_T off;
            regions++;
            bytes += (DWORD)(size / 1024);
            /* A definition must have DEF_DUMP bytes behind it to be dumped, so
             * stop far enough from the end of the region. */
            for (off = 0; off + DEF_DUMP <= size; off += 4) {
                DWORD *p = (DWORD *)(base + off);
                if (*p != CUIDEF_VTABLE) continue;
                if (g_found_n < MAX_FOUND) {
                    g_found[g_found_n].def  = (DWORD)(uintptr_t)p;
                    g_found[g_found_n].id   = p[DEF_ID / 4];
                    g_found[g_found_n].type = p[DEF_TYPE / 4];
                    g_found_n++;
                }
            }
        }
        addr = base + size;
        if (size == 0) break;
    }
    plog("scanned %lu readable region(s), ~%lu MB; %d definition(s) found",
         regions, bytes / 1024, g_found_n);
}

static DWORD def_by_id(DWORD id)
{
    int i;
    for (i = 0; i < g_found_n; i++)
        if (g_found[i].id == id) return g_found[i].def;
    return 0;
}

/* --- decoding --- */

static int read_text(DWORD p, char *out, int cap)
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

static int field_text(DWORD v, char *out, int cap)
{
    DWORD inner;
    if (read_text(v, out, cap)) return 1;
    if (!PLAUSIBLE(v) || IsBadReadPtr((void *)(uintptr_t)v, 4)) return 0;
    inner = *(DWORD *)(uintptr_t)v;
    if (read_text(inner, out, cap)) return 2;
    return 0;
}

static int floatish(DWORD v)
{
    float f;
    DWORD exp = (v >> 23) & 0xFF;
    memcpy(&f, &v, 4);
    if (v == 0 || exp == 0 || exp == 0xFF) return 0;
    return (f > -100000.0f && f < 100000.0f);
}

static void describe(DWORD v, char *out, int cap)
{
    char text[64];
    int hop = field_text(v, text, sizeof text);

    if (hop)                { snprintf(out, cap, "\"%.20s\"", text); }
    else if (floatish(v))   { float f; memcpy(&f, &v, 4);
                              snprintf(out, cap, "%.2ff", (double)f); }
    else if (v < 100000)      snprintf(out, cap, "%lu", v);
    else if (PLAUSIBLE(v))    snprintf(out, cap, "ptr");
    else                      out[0] = 0;
}

static DWORD at(DWORD def, int off)
{
    return *(DWORD *)(uintptr_t)(def + off);
}

/* Column diff across the six label definitions. */
static void diff(const char *what, const DWORD *objs, int size)
{
    int off;

    plog("");
    plog("--- %s: DIFFERS across the six rows ---", what);
    plog("  %-7s %-13s %-13s %-13s %-13s %-13s %-13s", "offset",
         ROW_NAME[0], ROW_NAME[1], ROW_NAME[2],
         ROW_NAME[3], ROW_NAME[4], ROW_NAME[5]);

    for (off = 0; off + 4 <= size; off += 4) {
        DWORD v[ROWS];
        char d[ROWS][32];
        int i, same = 1;

        for (i = 0; i < ROWS; i++) {
            v[i] = at(objs[i], off);
            if (i && v[i] != v[0]) same = 0;
        }
        if (same) continue;
        for (i = 0; i < ROWS; i++) describe(v[i], d[i], sizeof d[i]);
        plog("  +0x%03X %-13s %-13s %-13s %-13s %-13s %-13s",
             off, d[0], d[1], d[2], d[3], d[4], d[5]);
        plog("        %-13lX %-13lX %-13lX %-13lX %-13lX %-13lX",
             v[0], v[1], v[2], v[3], v[4], v[5]);
    }

    plog("");
    plog("--- %s: SHARED by all six ---", what);
    for (off = 0; off + 4 <= size; off += 4) {
        DWORD first = at(objs[0], off);
        char d[32];
        int i, same = 1;
        for (i = 1; i < ROWS; i++)
            if (at(objs[i], off) != first) { same = 0; break; }
        if (!same || first == 0) continue;
        describe(first, d, sizeof d);
        plog("  +0x%03X  %08lX  %s", off, first, d);
    }
}

static void dump_states(const char *what, const DWORD *objs)
{
    int s;

    for (s = 0; s < 8; s++) {
        DWORD st[ROWS];
        int i, all = 1;

        for (i = 0; i < ROWS; i++) {
            DWORD b = at(objs[i], DEF_STATES), e = at(objs[i], DEF_STATES_END);
            DWORD n = (PLAUSIBLE(b) && e > b) ? (e - b) / STATE_SIZE : 0;
            if (n <= (DWORD)s) { all = 0; break; }
            st[i] = b + (DWORD)s * STATE_SIZE;
        }
        if (!all) break;
        {
            char title[48];
            snprintf(title, sizeof title, "%s CUIStateDef[%d]", what, s);
            diff(title, st, STATE_SIZE);
        }
    }
}

/* Walk a definition's children, depth first, printing the tree. Collects the
 * first CText (type 6) it reaches, which is where a label's string must be. */
static DWORD walk(DWORD def, int depth, DWORD *first_text)
{
    DWORD b, e, n, k, type;
    char pad[24];
    int i;

    if (!def || depth > 5) return 0;
    type = at(def, DEF_TYPE);
    b = at(def, DEF_CHILD_BEGIN);
    e = at(def, DEF_CHILD_END);
    n = (PLAUSIBLE(b) && e > b && (e - b) < 0x200) ? (e - b) / 4 : 0;

    for (i = 0; i < depth && i < 10; i++) { pad[i * 2] = ' '; pad[i * 2 + 1] = ' '; }
    pad[depth * 2] = 0;

    plog("      %s id %-5lu type %-3lu  %lu state(s)  %lu child(ren)%s",
         pad, at(def, DEF_ID), type,
         (at(def, DEF_STATES_END) - at(def, DEF_STATES)) / STATE_SIZE, n,
         type == 6 ? "   <- CText" : "");

    if (type == 6 && !*first_text) *first_text = def;

    for (k = 0; k < n; k++)
        walk(def_by_id(*(DWORD *)(uintptr_t)(b + k * 4)), depth + 1, first_text);
    return 0;
}

static void report(void)
{
    DWORD labels[ROWS], buttons[ROWS];
    int k, have = 0;

    plog("");
    plog("=== the six rows ===");
    for (k = 0; k < ROWS; k++) {
        buttons[k] = def_by_id(ROW_BUTTON[k]);
        labels[k]  = def_by_id(ROW_LABEL[k]);
        plog("  %-14s button id %-4lu def 0x%08lX   label id %-4lu def 0x%08lX%s",
             ROW_NAME[k], ROW_BUTTON[k], buttons[k], ROW_LABEL[k], labels[k],
             labels[k] ? "" : "   <- NOT FOUND");
        if (labels[k]) have++;
    }
    if (have != ROWS) {
        plog("only %d of %d label definitions found -- diff skipped", have, ROWS);
        return;
    }

    plog("");
    plog("=== label definition types ===");
    for (k = 0; k < ROWS; k++) {
        DWORD b = at(labels[k], DEF_STATES), e = at(labels[k], DEF_STATES_END);
        plog("  %-14s type %lu, %lu state(s), %lu child(ren)",
             ROW_NAME[k], at(labels[k], DEF_TYPE),
             (PLAUSIBLE(b) && e > b) ? (e - b) / STATE_SIZE : 0,
             (at(labels[k], DEF_CHILD_END) - at(labels[k], DEF_CHILD_BEGIN)) / 4);
    }

    plog("");
    plog("=== the definition tree under each row ===");
    {
        DWORD texts[ROWS];
        int found = 0;

        for (k = 0; k < ROWS; k++) {
            plog("  %s:", ROW_NAME[k]);
            texts[k] = 0;
            walk(buttons[k], 0, &texts[k]);
            if (texts[k]) found++;
        }

        plog("");
        plog("=== the CText under each row ===");
        for (k = 0; k < ROWS; k++)
            plog("  %-14s CText def 0x%08lX id %lu", ROW_NAME[k], texts[k],
                 texts[k] ? at(texts[k], DEF_ID) : 0);

        if (found == ROWS) {
            diff("row CText CUIDef", texts, 0x140);
            dump_states("row CText", texts);

            plog("");
            plog("=== strings reachable from each row's CText ===");
            for (k = 0; k < ROWS; k++) {
                int off;
                plog("  %s:", ROW_NAME[k]);
                for (off = 0; off + 4 <= 0x140; off += 4) {
                    char text[96];
                    if (field_text(at(texts[k], off), text, sizeof text))
                        plog("    +0x%03X \"%s\"", off, text);
                }
            }
        } else {
            plog("  only %d of %d rows reached a CText", found, ROWS);
        }
    }

    plog("");
    plog("=== strings reachable from each label definition ===");
    for (k = 0; k < ROWS; k++) {
        int off;
        plog("  %s (id %lu):", ROW_NAME[k], ROW_LABEL[k]);
        for (off = 0; off + 4 <= DEF_DUMP; off += 4) {
            char text[96];
            if (field_text(at(labels[k], off), text, sizeof text))
                plog("    +0x%03X \"%s\"", off, text);
        }
    }
}

static DWORD WINAPI worker(LPVOID unused)
{
    (void)unused;
    g_log = fopen(LOG_PATH, "w");
    plog("=== menu-label: what selects a menu row's text ===");
    plog("read-only; definitions found by scanning for the CUIDef vtable "
         "0x%08X", CUIDEF_VTABLE);

    scan();
    report();
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
