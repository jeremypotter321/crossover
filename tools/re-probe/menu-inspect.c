/*
 * menu-inspect: read the main menu's six button definitions and diff them.
 *
 * Read-only. Nothing is patched, nothing is called into. This exists to answer
 * one question about the seventh entry proved in docs/ui-system.md section 13:
 * which bytes of a CFrontEndButton definition carry its **text** and its
 * **action**, so that an appended row can be made ours instead of a duplicate
 * of a stock one.
 *
 * The method is a same-type diff, which section 8 records as the open item --
 * the earlier capture run collected only one button definition, so there was
 * nothing to diff it against. That is no longer true: section 13 established
 * the main menu list (definition id 245) and its six children,
 *
 *     262  247  266  280  255  251
 *     |    |    |    |    |    `- Quit
 *     |    |    |    |    `- About
 *     |    |    |    `- Credits
 *     |    |    `- Options
 *     |    `- Change Profile
 *     `- Continue Game
 *
 * which are six definitions of the same type doing six different things. Any
 * field that is equal across all six is layout or styling shared by every menu
 * row. Any field that differs is a candidate for the two things we want, and
 * the diff prints each differing field decoded as integer, float and -- when it
 * looks like a pointer to text -- as the string it reaches.
 *
 * Capture happens at the same one-instruction window as before (0x0041D249,
 * `mov eax,[ebx+0x3C]`, EBX = the definition), with the handler emulating the
 * instruction so the breakpoint never has to leave the code and no definition
 * in the construction burst is missed.
 *
 * The handler only memcpy's. All decoding, pointer-chasing and logging happens
 * on the worker thread afterwards: doing any of it inside the game's own
 * dispatch is what killed the process in earlier probes.
 */

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#define LOG_PATH "menu-inspect2.log"

/* --- definition layout (docs/ui-system.md sections 8-13) --- */
#define DEF_ID          0x20
#define DEF_TYPE        0x3C
#define DEF_STATES      0x40      /* std::vector<CUIStateDef> begin */
#define DEF_STATES_END  0x44
#define DEF_CHILD_BEGIN 0x70
#define DEF_CHILD_END   0x74

#define TYPE_BUTTON     0x0B
#define TYPE_LIST       0x0C
#define TYPE_TEXT       0x06

#define STATE_SIZE      0x7C      /* sizeof(CUIStateDef) */

#define FACTORY_TYPE_READ 0x0041D249u
#define FACTORY_TYPE_LEN  3

/* The main menu list, and its children in rendered order. */
#define MENU_LIST_ID 245
static const DWORD MENU_IDS[] = { 262, 247, 266, 280, 255, 251 };
static const char *MENU_NAMES[] = {
    "Continue Game", "Change Profile", "Options", "Credits", "About", "Quit"
};
#define MENU_N ((int)(sizeof MENU_IDS / sizeof MENU_IDS[0]))

#define DEF_DUMP   0x200
#define STATE_DUMP STATE_SIZE
#define MAX_STATES 8
#define MAX_CAPS   160

typedef struct {
    DWORD def;
    DWORD id;
    DWORD type;
    unsigned char raw[DEF_DUMP];
    unsigned char states[MAX_STATES][STATE_DUMP];
    DWORD nstates;
    DWORD kids[16];
    DWORD nkids;
} CapDef;

static CapDef g_caps[MAX_CAPS];
static volatile LONG g_caps_n;

/*
 * The six menu buttons get reserved slots.
 *
 * A shared pool does not work: the game builds a great many definitions before
 * it ever reaches the main menu, so a first-come pool is full long before the
 * rows we care about are constructed, and the run then waits forever for a
 * capture that can no longer happen. These slots cannot be crowded out.
 */
static CapDef g_menu[MENU_N];
static volatile LONG g_menu_have;

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

/* Copy only. Every interpretation of these bytes happens on the worker. */
static void capture(DWORD def)
{
    DWORD type, b, e;
    LONG i, idx;

    CapDef *c = NULL;
    DWORD id;

    if (!PLAUSIBLE(def)) return;
    type = *(DWORD *)(uintptr_t)(def + DEF_TYPE);
    id = *(DWORD *)(uintptr_t)(def + DEF_ID);

    for (i = 0; i < MENU_N; i++) {
        if (id == MENU_IDS[i] && type == TYPE_BUTTON) {
            if (g_menu[i].def) return;             /* already have it */
            c = &g_menu[i];
            g_menu_have++;
            break;
        }
    }

    if (!c) {
        /* Only the types that can plausibly carry a label or hold one. */
        if (type != TYPE_BUTTON && type != TYPE_LIST && type != TYPE_TEXT)
            return;
        for (i = 0; i < g_caps_n; i++)
            if (g_caps[i].def == def) return;
        idx = g_caps_n;
        if (idx >= MAX_CAPS) return;
        g_caps_n = idx + 1;
        c = &g_caps[idx];
    }

    c->def  = def;
    c->id   = id;
    c->type = type;
    memcpy(c->raw, (const void *)(uintptr_t)def, DEF_DUMP);

    b = *(DWORD *)(uintptr_t)(def + DEF_STATES);
    e = *(DWORD *)(uintptr_t)(def + DEF_STATES_END);
    if (PLAUSIBLE(b) && e > b && (e - b) % STATE_SIZE == 0) {
        DWORD n = (e - b) / STATE_SIZE, k;
        if (n > MAX_STATES) n = MAX_STATES;
        c->nstates = n;
        for (k = 0; k < n; k++)
            memcpy(c->states[k],
                   (const void *)(uintptr_t)(b + k * STATE_SIZE), STATE_DUMP);
    }

    /* A button's own children -- the label is far more likely to be a CText
     * child than a scalar in the button, which is how the menu rows themselves
     * are children of a list. */
    b = *(DWORD *)(uintptr_t)(def + DEF_CHILD_BEGIN);
    e = *(DWORD *)(uintptr_t)(def + DEF_CHILD_END);
    if (PLAUSIBLE(b) && e > b && (e - b) <= 16 * 4) {
        DWORD n = (e - b) / 4, k;
        c->nkids = n;
        for (k = 0; k < n; k++)
            c->kids[k] = *(DWORD *)(uintptr_t)(b + k * 4);
    }
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

    capture(ebx);

    /* `mov eax,[ebx+0x3C]` performed here; three bytes, no flags written. */
    ep->ContextRecord->Eax = *(DWORD *)(uintptr_t)(ebx + DEF_TYPE);
    ep->ContextRecord->Eip = FACTORY_TYPE_READ + FACTORY_TYPE_LEN;
    return EXCEPTION_CONTINUE_EXECUTION;
}

/* --- decoding, all on the worker thread --- */

/* Read printable ASCII at p. Returns 0 unless it looks like real text. */
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
    return i >= 3;                       /* three characters or it is noise */
}

/*
 * A CharString is a pointer to a pointer to the characters -- the game's own
 * accessor for a button's name is *(char **)*(DWORD *)(obj + 0x20). So a field
 * holding text may be the text, or one hop away from it.
 */
static int field_text(DWORD v, char *out, int cap)
{
    DWORD inner;

    if (read_text(v, out, cap)) return 1;
    if (!PLAUSIBLE(v) || IsBadReadPtr((void *)(uintptr_t)v, 4)) return 0;
    inner = *(DWORD *)(uintptr_t)v;
    if (read_text(inner, out, cap)) return 2;
    return 0;
}

static const CapDef *cap_by_id(DWORD id)
{
    LONG i;
    for (i = 0; i < MENU_N; i++)
        if (g_menu[i].def && g_menu[i].id == id) return &g_menu[i];
    for (i = 0; i < g_caps_n; i++)
        if (g_caps[i].id == id) return &g_caps[i];
    return NULL;
}

static DWORD dw(const unsigned char *raw, int off)
{
    DWORD v;
    memcpy(&v, raw + off, 4);
    return v;
}

/* A float worth printing: finite, not absurd, and not obviously an integer id. */
static int floatish(DWORD v)
{
    float f;
    DWORD exp = (v >> 23) & 0xFF;
    memcpy(&f, &v, 4);
    if (v == 0) return 0;
    if (exp == 0 || exp == 0xFF) return 0;
    return (f > -100000.0f && f < 100000.0f);
}

static void describe(DWORD v, char *out, int cap)
{
    char text[96];
    int hop = field_text(v, text, sizeof text);

    if (hop == 1)      snprintf(out, cap, "-> \"%s\"", text);
    else if (hop == 2) snprintf(out, cap, "-> -> \"%s\"", text);
    else if (floatish(v)) {
        float f;
        memcpy(&f, &v, 4);
        snprintf(out, cap, "%.2ff", (double)f);
    } else if (v < 100000)  snprintf(out, cap, "%lu", v);
    else if (PLAUSIBLE(v))  snprintf(out, cap, "ptr");
    else                    out[0] = 0;
}

static void diff_block(const char *what, const CapDef *caps[], int n, int size,
                       int state)
{
    int off;

    plog("");
    plog("--- %s: fields that DIFFER across the six menu buttons ---", what);
    plog("  %-6s %-14s %-14s %-14s %-14s %-14s %-14s",
         "offset", MENU_NAMES[0], MENU_NAMES[1], MENU_NAMES[2],
         MENU_NAMES[3], MENU_NAMES[4], MENU_NAMES[5]);

    for (off = 0; off + 4 <= size; off += 4) {
        DWORD v[MENU_N];
        char d[MENU_N][40];
        int i, same = 1;

        for (i = 0; i < n; i++) {
            const unsigned char *raw = state ? caps[i]->states[state - 1]
                                             : caps[i]->raw;
            v[i] = dw(raw, off);
            if (i && v[i] != v[0]) same = 0;
        }
        if (same) continue;

        for (i = 0; i < n; i++) describe(v[i], d[i], sizeof d[i]);
        plog("  +0x%03X %-14s %-14s %-14s %-14s %-14s %-14s", off,
             d[0], d[1], d[2], d[3], d[4], d[5]);
        plog("         %-14lX %-14lX %-14lX %-14lX %-14lX %-14lX",
             v[0], v[1], v[2], v[3], v[4], v[5]);
    }

    plog("");
    plog("--- %s: fields SHARED by all six (layout/styling, not identity) ---", what);
    for (off = 0; off + 4 <= size; off += 4) {
        DWORD first = dw(state ? caps[0]->states[state - 1] : caps[0]->raw, off);
        char d[40];
        int i, same = 1;

        for (i = 1; i < n; i++)
            if (dw(state ? caps[i]->states[state - 1] : caps[i]->raw, off) != first)
                { same = 0; break; }
        if (!same || first == 0) continue;
        describe(first, d, sizeof d);
        plog("  +0x%03X  %08lX  %s", off, first, d);
    }
}

static void report(void)
{
    const CapDef *caps[MENU_N];
    LONG i;
    int k, have = 0;

    plog("");
    plog("=== captured %ld definition(s) ===", g_caps_n);
    for (i = 0; i < g_caps_n; i++)
        plog("  %-6s def 0x%08lX id %-5lu states %lu",
             g_caps[i].type == TYPE_LIST ? "list" : "button",
             g_caps[i].def, g_caps[i].id, g_caps[i].nstates);

    for (k = 0; k < MENU_N; k++) {
        caps[k] = cap_by_id(MENU_IDS[k]);
        if (caps[k]) have++;
        else plog("  MISSING: %s (id %lu) was not captured",
                  MENU_NAMES[k], MENU_IDS[k]);
    }
    if (have != MENU_N) {
        plog("only %d of %d menu buttons captured -- diff skipped", have, MENU_N);
        return;
    }

    plog("");
    plog("=== the six main-menu button definitions ===");
    for (k = 0; k < MENU_N; k++)
        plog("  %-14s id %-5lu def 0x%08lX  %lu state(s)",
             MENU_NAMES[k], caps[k]->id, caps[k]->def, caps[k]->nstates);

    diff_block("CUIDef", caps, MENU_N, DEF_DUMP, 0);
    for (k = 0; k < MAX_STATES; k++) {
        char title[32];
        int j, all = 1;
        for (j = 0; j < MENU_N; j++) if (caps[j]->nstates <= (DWORD)k) all = 0;
        if (!all) continue;
        snprintf(title, sizeof title, "CUIStateDef[%d]", k);
        diff_block(title, caps, MENU_N, STATE_DUMP, k + 1);
    }

    plog("");
    plog("=== each menu button's own child definitions ===");
    for (k = 0; k < MENU_N; k++) {
        char line[256];
        int off = 0;
        DWORD j;
        off += snprintf(line + off, sizeof line - off, "  %-14s id %-5lu: ",
                        MENU_NAMES[k], caps[k]->id);
        if (!caps[k]->nkids)
            snprintf(line + off, sizeof line - off, "(no children)");
        else
            for (j = 0; j < caps[k]->nkids && off < (int)sizeof line - 24; j++) {
                const CapDef *kid = NULL;
                LONG q;
                kid = cap_by_id(caps[k]->kids[j]);
                (void)q;
                off += snprintf(line + off, sizeof line - off, "%lu(type %s) ",
                                caps[k]->kids[j],
                                kid ? (kid->type == 6 ? "6=CText" : "?") : "uncaught");
            }
        plog("%s", line);
    }

    plog("");
    plog("=== every definition captured, by type ===");
    for (i = 0; i < g_caps_n; i++)
        plog("  type %2lu  def 0x%08lX  id %-5lu  states %lu  kids %lu",
             g_caps[i].type, g_caps[i].def, g_caps[i].id,
             g_caps[i].nstates, g_caps[i].nkids);

    plog("");
    plog("=== every string reachable from each definition ===");
    for (k = 0; k < MENU_N; k++) {
        int off;
        plog("  %s (id %lu):", MENU_NAMES[k], caps[k]->id);
        for (off = 0; off + 4 <= DEF_DUMP; off += 4) {
            char text[96];
            DWORD v = dw(caps[k]->raw, off);
            int hop = field_text(v, text, sizeof text);
            if (hop) plog("    +0x%03X %s\"%s\"", off,
                          hop == 2 ? "-> " : "", text);
        }
    }
}

/*
 * Wine's intermittent 0x80040218 video-playback dialog.
 *
 * It is a game screen with its own OK button, drawn in front of everything, and
 * while it is up the main menu is never constructed -- so a probe waiting for
 * the menu's definitions waits forever. This is the same dialog docs/ui-system.md
 * section 12 records as the reason the child-list result went unphotographed.
 *
 * The window cannot be clicked from macOS (osascript has no accessibility
 * permission here) but we are inside the process, so the keystroke is posted
 * straight to the game's own window.
 */
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
    int reported = 0;

    (void)unused;
    g_log = fopen(LOG_PATH, "w");
    plog("=== menu-inspect: what carries a menu button's text and action ===");
    plog("read-only; the six main-menu button definitions, diffed against "
         "each other");

    AddVectoredExceptionHandler(1, bp_handler);
    plog("breakpoint at 0x%08X: %s", FACTORY_TYPE_READ,
         arm_bp() ? "armed" : "FAILED");

    for (t = 0; t < 100000; t++) {
        /* Report once the menu has been built and gone quiet. */
        if (!reported && t > 200) {
            int k, have = 0;
            for (k = 0; k < MENU_N; k++) if (cap_by_id(MENU_IDS[k])) have++;
            if (have == MENU_N) {
                disarm_bp();
                report();
                reported = 1;
                plog("");
                plog("=== breakpoint removed; the game is back to its own code ===");
            }
        }
        if (!reported && (t & 0x3F) == 0) dismiss_dialogs();
        Sleep(20);
    }

    if (!reported) { disarm_bp(); report(); }
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
