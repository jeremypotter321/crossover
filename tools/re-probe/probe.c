/*
 * re-probe: hook/patch harness for Fable's frontend, built from macOS.
 *
 * The real mod can only be built with MSVC (SLikeNet is a prebuilt MSVC C++
 * static lib), but a standalone probe has no such dependency and cross-compiles
 * cleanly with mingw-w64, giving a build/inject/observe loop that runs locally
 * under Wine instead of round-tripping through Windows CI.
 *
 * What was established by the earlier read-only passes:
 *   - Fable.exe has no ASLR; it always loads at 0x400000.
 *   - A CFrontEndButton carries its definition name at
 *       *(char **)*(DWORD *)(button + 0x20)
 *   - A CFrontEndList owns its children in a std::vector<CFrontEndButton *>
 *     whose {begin,end,capacity} triple lives at list+0x164/+0x168/+0x16C.
 *   - The main menu is the list holding UI_FRONTEND_BUTTON_QUIT.
 *
 * This pass ADDS an entry to the main menu by cloning an existing button and
 * appending it to that vector.
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>

#define LOG_PATH "probe.log"

#define VT_FRONTEND_SCREEN 0x012497E4u
#define VT_FRONTEND_BUTTON 0x01249554u
#define VT_FRONTEND_LIST   0x01249224u

#define VEC_BEGIN 0x164
#define VEC_END   0x168
#define VEC_CAP   0x16C

#define BUTTON_CLONE_BYTES 0x400   /* ctor touches +0x1B0, so the object exceeds it */
#define MAX_OBJ 64

static FILE *g_log;
static void *g_own_stack;
static int   g_y_off = -1;      /* offset of the button's vertical position float */
static float g_y_step = 0.0f;   /* spacing between menu rows */

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
    /* Include MEM_IMAGE (the exe's own .data) and MEM_MAPPED, not just the
     * heap -- a global draw list lives in .data and earlier passes, which
     * filtered on MEM_PRIVATE, could never have seen it. */
    return mbi->State == MEM_COMMIT &&
           !(mbi->Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
           (mbi->Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE |
                            PAGE_READONLY | PAGE_EXECUTE_READ |
                            PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY));
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

/* Definition name of a button, or NULL. */
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

static void identify_owner(DWORD site)
{
    static const struct { const char *name; DWORD vt; } kinds[] = {
        { "CFrontEndManager", 0x012521A8u },
        { "CFrontEndScreen",  VT_FRONTEND_SCREEN },
        { "CFrontEndButton",  VT_FRONTEND_BUTTON },
        { "CFrontEndList",    VT_FRONTEND_LIST   },
    };
    DWORD a;
    for (a = site & ~3u; a > site - 0x4000 && a > 0x10000; a -= 4) {
        DWORD v;
        unsigned k;
        if (IsBadReadPtr((void *)(uintptr_t)a, 4)) break;
        v = *(DWORD *)(uintptr_t)a;
        for (k = 0; k < sizeof kinds / sizeof kinds[0]; k++)
            if (v == kinds[k].vt) {
                plog("        owner %-17s 0x%08lX  at +0x%lX",
                     kinds[k].name, a, site - a);
                return;
            }
    }
    plog("        owner unidentified (no vtable within 0x4000)");
}

static DWORD WINAPI probe_main(LPVOID unused)
{
    DWORD lists[MAX_OBJ], refs[MAX_OBJ];
    int nl, i, j, nrefs;
    int marker;
    DWORD menu = 0, quit_btn = 0, credits_btn = 0;

    (void)unused;
    g_own_stack = &marker;
    g_log = fopen(LOG_PATH, "w");
    if (!g_log) return 0;

    plog("=== re-probe attached (referrer/owner map) ===");
    Sleep(40000);

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
            if (strcmp(nm, "UI_FRONTEND_BUTTON_QUIT") == 0) { menu = lists[i]; quit_btn = btn; }
            if (strcmp(nm, "UI_FRONTEND_BUTTON_CREDITS") == 0) credits_btn = btn;
        }
    }
    if (!quit_btn) { plog("!! Quit button not found"); goto done; }
    plog("menu list 0x%08lX  quit=0x%08lX  credits=0x%08lX", menu, quit_btn, credits_btn);

    plog("");
    plog("=== ANY pointer landing inside the Quit button (intrusive links) ===");
    {
        MEMORY_BASIC_INFORMATION mbi;
        unsigned char *addr = NULL;
        DWORD lo = quit_btn, hi = quit_btn + 0x400;
        int n = 0;
        while (VirtualQuery(addr, &mbi, sizeof mbi) == sizeof mbi) {
            unsigned char *next = (unsigned char *)mbi.BaseAddress + mbi.RegionSize;
            if (region_ok(&mbi)) {
                unsigned char *q = (unsigned char *)mbi.BaseAddress, *e2 = next - 4;
                for (; q <= e2; q += 4) {
                    DWORD v = *(DWORD *)q;
                    if (v >= lo && v < hi) {
                        DWORD site = (DWORD)(uintptr_t)q;
                        if (site >= lo && site < hi) continue;   /* self-links */
                        if (n++ < 40)
                            plog("    0x%08lX -> quit+0x%03lX", site, v - quit_btn);
                    }
                }
            }
            if (next <= addr) break;
            addr = next;
        }
        plog("  total external pointers into the button: %d", n);
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
