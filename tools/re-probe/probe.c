/*
 * hero-probe: find the tutorial hero's definition so the guild seal can be
 * added to whatever item list it carries.
 *
 * Established for UI definitions (docs/ui-system.md): a definition is an object
 * beginning with a vtable in .rdata, and objects reference their definition
 * name through a CharString. The same shape should hold for creature
 * definitions, so this locates CREATURE_HERO_CHILD by finding the live copy of
 * its name and walking back to whatever points at it.
 *
 * Read-only.
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>

#define LOG_PATH "probe.log"
#define RDATA_LO 0x0122D000u
#define RDATA_HI 0x01374000u
#define MAX_HITS 64

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
           (mbi->Protect & (PAGE_READWRITE | PAGE_READONLY |
                            PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE));
}

/* Find NUL-terminated occurrences of `s` in live memory. */
static int find_string(const char *s, int len, DWORD *out, int max_out)
{
    MEMORY_BASIC_INFORMATION mbi;
    unsigned char *addr = NULL;
    int found = 0;
    while (VirtualQuery(addr, &mbi, sizeof mbi) == sizeof mbi) {
        unsigned char *next = (unsigned char *)mbi.BaseAddress + mbi.RegionSize;
        if (region_ok(&mbi)) {
            unsigned char *p = (unsigned char *)mbi.BaseAddress, *e = next - len - 1;
            for (; p <= e; p++) {
                if (*p != (unsigned char)s[0]) continue;
                if (memcmp(p, s, len) == 0 && p[len] == 0) {
                    if (found < max_out) out[found] = (DWORD)(uintptr_t)p;
                    found++;
                }
            }
        }
        if (next <= addr) break;
        addr = next;
    }
    return found;
}

static int scan_dword(DWORD v, DWORD *out, int max_out)
{
    MEMORY_BASIC_INFORMATION mbi;
    unsigned char *addr = NULL;
    int found = 0;
    while (VirtualQuery(addr, &mbi, sizeof mbi) == sizeof mbi) {
        unsigned char *next = (unsigned char *)mbi.BaseAddress + mbi.RegionSize;
        if (region_ok(&mbi)) {
            unsigned char *p = (unsigned char *)mbi.BaseAddress, *e = next - 4;
            for (; p <= e; p += 4)
                if (*(DWORD *)p == v) {
                    if (found < max_out) out[found] = (DWORD)(uintptr_t)p;
                    found++;
                }
        }
        if (next <= addr) break;
        addr = next;
    }
    return found;
}

static void report(const char *name)
{
    DWORD hits[MAX_HITS];
    int n, i;

    n = find_string(name, (int)strlen(name), hits, MAX_HITS);
    plog("");
    plog("=== \"%s\": %d live copy(ies) ===", name, n);
    for (i = 0; i < n && i < 6; i++) {
        DWORD refs[MAX_HITS];
        int nr = scan_dword(hits[i], refs, MAX_HITS), j;
        plog("  copy @0x%08lX  referenced %d time(s)", hits[i], nr);
        for (j = 0; j < nr && j < 5; j++) {
            /* walk back for a vtable: that is the owning definition */
            DWORD a, owner = 0, off = 0;
            for (a = refs[j] & ~3u; a > refs[j] - 0x200 && a > 0x10000; a -= 4) {
                DWORD v = *(DWORD *)(uintptr_t)a;
                if (v >= RDATA_LO && v < RDATA_HI) { owner = a; off = refs[j] - a; break; }
            }
            if (owner)
                plog("      ref @0x%08lX -> object 0x%08lX (vtable 0x%08lX) at +0x%lX",
                     refs[j], owner, *(DWORD *)(uintptr_t)owner, off);
            else
                plog("      ref @0x%08lX -> no vtable within 0x200", refs[j]);
        }
    }
}

static DWORD WINAPI probe_main(LPVOID unused)
{
    int marker, t;
    (void)unused;
    g_own_stack = &marker;
    g_log = fopen(LOG_PATH, "w");
    if (!g_log) return 0;

    plog("=== hero-probe: locate the tutorial hero definition ===");

    /* Sample repeatedly: the hero definition only exists once a game is
     * actually loaded, which is well after the frontend. */
    for (t = 0; t < 20; t++) {
        Sleep(15000);
        plog("");
        plog("--- pass %d (t=%ds) ---", t + 1, (t + 1) * 15);
        report("CREATURE_HERO_CHILD");
        report("OBJECT_GUILD_SEAL_1");
    }

    plog("=== probe complete ===");
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
