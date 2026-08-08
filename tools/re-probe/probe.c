/*
 * re-probe: adds a "Crossover" entry to Fable's main menu, from macOS.
 *
 * Cross-compiled with mingw-w64 and injected under Wine, giving a local
 * build/inject/observe loop. (The full mod still needs MSVC because SLikeNet is
 * a prebuilt MSVC C++ static lib; this probe links nothing.)
 *
 * How it works -- and why it works this way:
 *
 *   Patching containers after the fact CANNOT add a menu entry. The
 *   CFrontEndList child vector at +0x164 is an ownership list, not the draw
 *   source: shrinking it still left the dropped entry rendering. The drawn set
 *   is fixed when the screen is constructed.
 *
 *   So instead this changes WHICH DEFINITION the menu is built from, before
 *   construction. sub_59899A selects the menu definition with a plain
 *   `push imm32`; rewriting that operand to a string of our own makes the game
 *   build a different menu through its own machinery. UI_FRONTEND_MAIN_MENU
 *   (the Xbox Live variant, unused on PC) carries one extra entry.
 *
 *   The label is then rewritten from "Xbox Live" to "Crossover" -- same length,
 *   so it is an in-place overwrite. It must run continuously from t=0 because
 *   the text is baked to glyphs at construction.
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>

#define LOG_PATH "probe.log"

#define VT_FRONTEND_LIST 0x01249224u

#define VEC_BEGIN 0x164
#define VEC_END   0x168
#define VEC_CAP   0x16C

/* Operands of the two `push imm32` menu-definition selections in sub_59899A. */
#define PUSH_IMM_NO_CONTINUE 0x005989E2u
#define PUSH_IMM_MAIN        0x005989E9u

#define SCAN_BYTES 0x400
#define MAX_OBJ 64

static FILE *g_log;
static void *g_own_stack;

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

static int region_ok(const MEMORY_BASIC_INFORMATION *mbi)
{
    unsigned char *base = (unsigned char *)mbi->BaseAddress;
    unsigned char *next = base + mbi->RegionSize;
    if (g_own_stack && (unsigned char *)g_own_stack >= base &&
        (unsigned char *)g_own_stack < next)
        return 0;
    return mbi->State == MEM_COMMIT &&
           !(mbi->Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
           (mbi->Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE |
                            PAGE_READONLY | PAGE_EXECUTE_READ));
}

static int scan_dword(DWORD value, DWORD *out, int max_out)
{
    MEMORY_BASIC_INFORMATION mbi;
    unsigned char *addr = NULL;
    int found = 0;
    while (VirtualQuery(addr, &mbi, sizeof mbi) == sizeof mbi) {
        unsigned char *next = (unsigned char *)mbi.BaseAddress + mbi.RegionSize;
        if (region_ok(&mbi)) {
            unsigned char *p = (unsigned char *)mbi.BaseAddress, *end = next - 4;
            for (; p <= end; p += 4)
                if (*(DWORD *)p == value) {
                    if (found < max_out) out[found] = (DWORD)(uintptr_t)p;
                    found++;
                }
        }
        if (next <= addr) break;
        addr = next;
    }
    return found;
}

static const char *button_defname(DWORD btn)
{
    DWORD holder, str;
    if (IsBadReadPtr((void *)(uintptr_t)(btn + 0x20), 4)) return NULL;
    holder = *(DWORD *)(uintptr_t)(btn + 0x20);
    if (holder < 0x10000 || IsBadReadPtr((void *)(uintptr_t)holder, 4)) return NULL;
    str = *(DWORD *)(uintptr_t)holder;
    if (str < 0x10000 || IsBadReadPtr((void *)(uintptr_t)str, 8)) return NULL;
    return (const char *)(uintptr_t)str;
}

static int patch_dword(DWORD at, DWORD value)
{
    DWORD old;
    if (!VirtualProtect((void *)(uintptr_t)at, 4, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    *(DWORD *)(uintptr_t)at = value;
    VirtualProtect((void *)(uintptr_t)at, 4, old, &old);
    return 1;
}

/* Rewrite every live copy of the label. Patterns are assembled at runtime so
 * the scan cannot match (and corrupt) this module's own string literals. */
static void relabel_pass(const char *want, const char *repl, int len,
                         int *n_ascii, int *n_utf16)
{
    MEMORY_BASIC_INFORMATION mbi;
    unsigned char *addr = NULL;
    unsigned short want_w[32], repl_w[32];
    int i;

    for (i = 0; i < len; i++) {
        want_w[i] = (unsigned short)(unsigned char)want[i];
        repl_w[i] = (unsigned short)(unsigned char)repl[i];
    }

    while (VirtualQuery(addr, &mbi, sizeof mbi) == sizeof mbi) {
        unsigned char *next = (unsigned char *)mbi.BaseAddress + mbi.RegionSize;
        /* Never write into executable pages: making them PAGE_READWRITE strips
         * EXECUTE, and the game dies the moment it runs code there. Menu text
         * lives in data, so skipping code costs nothing. */
        /* Cover heap, mapped files and the exe's own data -- the copy the menu
         * actually reads is NOT on the heap (a heap-only sweep found 4 of the
         * 9 live copies and the label did not change). Executable regions are
         * excluded: making one PAGE_READWRITE strips EXECUTE and the game dies
         * on the next instruction fetch there. Menu text is never in code. */
        int candidate = mbi.State == MEM_COMMIT &&
                        !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
                        !(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                                         PAGE_EXECUTE_READWRITE |
                                         PAGE_EXECUTE_WRITECOPY)) &&
                        (mbi.Protect & (PAGE_READWRITE | PAGE_READONLY |
                                        PAGE_WRITECOPY)) != 0;
        if (candidate && !(g_own_stack &&
              (unsigned char *)g_own_stack >= (unsigned char *)mbi.BaseAddress &&
              (unsigned char *)g_own_stack < next)) {
            unsigned char *q = (unsigned char *)mbi.BaseAddress;
            unsigned char *e2 = next - (len * 2);
            unsigned char first = (unsigned char)want[0];
            for (; q <= e2; q++) {
                DWORD prot, back;
                /* Cheap reject: the overwhelming majority of bytes cannot start
                 * either encoding, and skipping the memcmp keeps each sweep
                 * light enough not to disturb the game. */
                if (*q != first) continue;
                if (memcmp(q, want, len) == 0) {
                    if (VirtualProtect(q, len, PAGE_READWRITE, &prot)) {
                        memcpy(q, repl, len);
                        VirtualProtect(q, len, prot, &back);  /* restore */
                        (*n_ascii)++;
                    }
                } else if (memcmp(q, want_w, len * 2) == 0) {
                    if (VirtualProtect(q, len * 2, PAGE_READWRITE, &prot)) {
                        memcpy(q, repl_w, len * 2);
                        VirtualProtect(q, len * 2, prot, &back);
                        (*n_utf16)++;
                    }
                }
            }
        }
        if (next <= addr) break;
        addr = next;
    }
}

static DWORD WINAPI probe_main(LPVOID unused)
{
    DWORD lists[MAX_OBJ];
    int nl, i, t, marker;
    int na = 0, nw = 0;
    char *defname;
    char want[16], repl[16];
    DWORD menu = 0, count = 0;
    DWORD ours = 0, ref = 0;

    (void)unused;
    g_own_stack = &marker;
    g_log = fopen(LOG_PATH, "w");
    if (!g_log) return 0;

    plog("=== re-probe: add \"Crossover\" to the main menu ===");

    /* 1. Point the menu-definition pushes at the 7-entry variant. */
    defname = (char *)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!defname) { plog("!! VirtualAlloc failed"); goto done; }
    strcpy(defname, "UI_FRONTEND_MAIN_MENU");
    plog("definition -> \"%s\" @ 0x%08lX", defname, (DWORD)(uintptr_t)defname);
    plog("patch no-continue operand: %s",
         patch_dword(PUSH_IMM_NO_CONTINUE, (DWORD)(uintptr_t)defname) ? "ok" : "FAILED");
    plog("patch main operand:        %s",
         patch_dword(PUSH_IMM_MAIN, (DWORD)(uintptr_t)defname) ? "ok" : "FAILED");

    /* 2. Rewrite the label continuously, so it is in place before the glyphs
     *    are baked at screen construction. */
    want[0]='X'; want[1]='b'; want[2]='o'; want[3]='x'; want[4]=' ';
    want[5]='L'; want[6]='i'; want[7]='v'; want[8]='e'; want[9]=0;
    repl[0]='C'; repl[1]='r'; repl[2]='o'; repl[3]='s'; repl[4]='s';
    repl[5]='o'; repl[6]='v'; repl[7]='e'; repl[8]='r'; repl[9]=0;

    /* Relabel continuously so it lands before the glyphs are baked at screen
     * construction. Restricted to heap memory: the live text buffers are on the
     * heap, and sweeping the mapped image as well was both slow and crashed the
     * game. */
    /* Construction time varies run to run (intro video length, save state), so
     * sweep from t=0 across a wide window. With the first-byte reject each pass
     * is cheap enough that this does not disturb the game. */
    for (t = 0; t < 110; t++) {
        relabel_pass(want, repl, 9, &na, &nw);
        Sleep(400);
    }
    plog("relabel: %d ascii + %d utf16 replacement(s)", na, nw);
    plog("relabel: %d ascii + %d utf16 replacement(s)", na, nw);

    /* 3. Locate our entry and a normal one, then diff them to find the field
     *    responsible for the larger text. */
    /* The menu is not always up at a fixed time, so retry rather than sample
     * once and give up. */
    for (t = 0; t < 40 && !(ours && ref); t++) {
    nl = scan_dword(VT_FRONTEND_LIST, lists, MAX_OBJ);
    if (nl > MAX_OBJ) nl = MAX_OBJ;

    for (i = 0; i < nl && !menu; i++) {
        DWORD b, e, n, k;
        if (IsBadReadPtr((void *)(uintptr_t)(lists[i] + VEC_CAP), 4)) continue;
        b = *(DWORD *)(uintptr_t)(lists[i] + VEC_BEGIN);
        e = *(DWORD *)(uintptr_t)(lists[i] + VEC_END);
        if (b < 0x10000 || e <= b || (e - b) % 4 || (e - b) > 0x400) continue;
        n = (e - b) / 4;
        for (k = 0; k < n; k++) {
            DWORD btn = *(DWORD *)(uintptr_t)(b + k * 4);
            const char *nm = button_defname(btn);
            if (!nm) continue;
            if (strcmp(nm, "UI_FRONTEND_BUTTON_LIVE_AWARE") == 0) {
                menu = lists[i]; count = n; ours = btn;
            }
            if (strcmp(nm, "UI_FRONTEND_BUTTON_CREDITS") == 0) ref = btn;
        }
    }

        if (!(ours && ref)) Sleep(2000);
    }

    if (!ours || !ref) { plog("!! could not locate both buttons"); goto done; }
    plog("");
    plog("our entry (LIVE_AWARE) = 0x%08lX", ours);
    plog("reference (CREDITS)    = 0x%08lX", ref);
    plog("menu list 0x%08lX, %lu children", menu, count);

    /*
     * NOT style fields: +0x00C and +0x158 were tried and rejected. Their values
     * swap between runs (ours=7/credits=1 one run, ours=1/credits=7 the next),
     * so they track ordering or transient state, not text style. Writing them
     * only perturbed the layout.
     */

    {
        float ours_y = *(float *)(uintptr_t)(ours + 0x38);
        DWORD b2 = *(DWORD *)(uintptr_t)(menu + VEC_BEGIN);
        DWORD e2 = *(DWORD *)(uintptr_t)(menu + VEC_END);
        DWORD k;
        int moved = 0;
        for (k = 0; k < (e2 - b2) / 4; k++) {
            DWORD btn = *(DWORD *)(uintptr_t)(b2 + k * 4);
            float *y;
            if (IsBadReadPtr((void *)(uintptr_t)(btn + 0x38), 4)) continue;
            y = (float *)(uintptr_t)(btn + 0x38);
            if (*y > ours_y) { *y += 22.0f; moved++; }
        }
        plog("");
        plog("overlap fix: our row y=%.1f, pushed %d lower entries down by 22",
             (double)ours_y, moved);
    }

    /* Keep the label rewritten while the menu is on screen, so it survives long
     * enough to be inspected. */
    for (t = 0; t < 60; t++) {
        relabel_pass(want, repl, 9, &na, &nw);
        Sleep(400);
    }
    plog("post-render relabel total: %d ascii + %d utf16", na, nw);

    plog("");
    plog("=== raw dwords differing between the two buttons ===");
    plog("    (position rows differ legitimately; looking for a style/font id)");
    for (i = 0; i + 4 <= 0x600; i += 4) {
        DWORD a, b;
        float fa, fb;
        if (IsBadReadPtr((void *)(uintptr_t)(ours + (DWORD)i), 4)) continue;
        if (IsBadReadPtr((void *)(uintptr_t)(ref + (DWORD)i), 4)) continue;
        a = *(DWORD *)(uintptr_t)(ours + (DWORD)i);
        b = *(DWORD *)(uintptr_t)(ref + (DWORD)i);
        if (a == b) continue;
        /* skip pointers -- they always differ and tell us nothing */
        if (a > 0x00400000u && b > 0x00400000u) continue;
        fa = *(float *)&a;
        fb = *(float *)&b;
        plog("    +0x%03X  ours=0x%08lX (%.3f)   credits=0x%08lX (%.3f)",
             i, a, (double)fa, b, (double)fb);
    }

done:
    plog("");
    plog("=== probe complete (game left running) ===");
    fclose(g_log);
    g_log = NULL;
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
