/*
 * console: is Lionhead's dev console live in the retail build?
 *
 * The whole console is compiled in (docs/ui-control-plan.md): CConsole is a
 * CTBaseSingleton, CInputProcessConsole is constructed at 0x0048B380 and
 * installed into slot +0x1F8 of the input-processor manager, and the command
 * registry (CommandList, VarList, BindKey, RunScript) sits at 0x009ED1F1
 * onwards. What is not yet known is whether any of it is instantiated at run
 * time or merely linked in.
 *
 * This answers that and nothing else. It is READ-ONLY: it scans committed
 * memory for objects whose first dword is one of the console vtables and dumps
 * their leading fields. No call into game code, no patched bytes, no
 * breakpoints -- every crash this project has caused came from one of those
 * three on the game's own thread, and none of them are needed to answer this.
 */

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>

#define LOG_PATH "C:\\Games\\Fable\\console.log"

/* Resolved from RTTI with tools/re-static/rtti.py. */
#define VT_CONSOLE          0x0129C600u  /* CConsole                    */
#define VT_CONSOLE_SINGLETON 0x0129C44Cu /* CTBaseSingleton<CConsole>   */
#define VT_INPUT_CONSOLE    0x01237F24u  /* CInputProcessConsole        */
#define VT_COMMAND_LINE     0x0129C400u  /* CConsoleCommandLine         */
#define VT_QUICKACCESS      0x01237AE0u  /* known-live control: the seal */

#define GSI_PTR 0x0143E8F8u

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

static int scan_vtable(DWORD vt, DWORD *out, int max_out)
{
    MEMORY_BASIC_INFORMATION mbi;
    unsigned char *addr = NULL;
    int n = 0;

    while (VirtualQuery(addr, &mbi, sizeof mbi) == sizeof mbi) {
        unsigned char *next = (unsigned char *)mbi.BaseAddress + mbi.RegionSize;
        int own = g_own_stack &&
                  (unsigned char *)g_own_stack >= (unsigned char *)mbi.BaseAddress &&
                  (unsigned char *)g_own_stack < next;
        if (!own && mbi.State == MEM_COMMIT &&
            !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
            (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE))) {
            unsigned char *p = (unsigned char *)mbi.BaseAddress;
            unsigned char *e = next - 0x80;
            for (; p <= e; p += 4)
                if (*(DWORD *)p == vt) {
                    if (n < max_out) out[n] = (DWORD)(uintptr_t)p;
                    n++;
                }
        }
        if (next <= addr) break;
        addr = next;
    }
    return n;
}

static void report(const char *label, DWORD vt)
{
    DWORD hits[8];
    int n = scan_vtable(vt, hits, 8), i, k;

    plog("");
    plog("%-24s vtable 0x%08X -> %d live instance(s)", label, vt, n);
    for (i = 0; i < n && i < 4; i++) {
        char line[128];
        int off = 0;
        /* Re-verify. The scan also matches the probe's own memory -- the
         * search value sits in this DLL's heap and stack -- and those matches
         * no longer hold the vtable by the time they are reported. */
        if (rd(hits[i]) != vt) { plog("    (0x%08lX: self-match, ignored)", hits[i]); continue; }
        plog("    instance @0x%08lX", hits[i]);
        for (k = 0; k < 0x30; k += 4) {
            off += sprintf(line + off, "%08lX ", rd(hits[i] + k));
            if ((k & 0x1C) == 0x1C) { plog("      +0x%02X  %s", k - 0x1C, line); off = 0; }
        }
    }
}

/*
 * CConsole is live even at the frontend. Walk it read-only looking for the
 * command registry -- whatever CommandList enumerates. Anything that looks
 * like a container (a pointer triple, or a pointer to a node whose fields
 * point at strings) is reported with any strings it reaches.
 */
static void dump_str_at(const char *tag, DWORD p)
{
    char buf[96];
    int k;
    if (!readable(p)) return;
    for (k = 0; k < 90; k++) {
        unsigned char c = *(unsigned char *)(uintptr_t)(p + k);
        if (c == 0) break;
        if (c < 0x20 || c > 0x7E) return;
        buf[k] = (char)c;
    }
    if (k < 3) return;
    buf[k] = 0;
    plog("        %s -> \"%s\"", tag, buf);
}

static void deep_console(void)
{
    DWORD hits[8];
    int n = scan_vtable(VT_CONSOLE, hits, 8), i, k;

    for (i = 0; i < n; i++) {
        DWORD c = hits[i];
        if (rd(c) != VT_CONSOLE) continue;
        plog("");
        plog("  --- CConsole @0x%08lX, 0x80 bytes ---", c);
        for (k = 0; k < 0x80; k += 4) {
            DWORD v = rd(c + k);
            plog("      +0x%02X = %08lX", k, v);
            dump_str_at("string", v);
            /* one level down: a container's first element */
            if (readable(v) && readable(rd(v))) dump_str_at("deref", rd(v));
        }
    }
}

/* Walk a candidate container: MSVC std::map/list nodes are {left,parent,right,
 * key,...}; std::vector is {begin,end,cap}. Report anything reachable that
 * looks like a command name. */
static void walk_container(const char *tag, DWORD p, int depth)
{
    int k;
    if (depth > 3 || !readable(p)) return;
    plog("    %s @0x%08lX:", tag, p);
    for (k = 0; k < 0x20; k += 4) {
        DWORD v = rd(p + k);
        if (!v) continue;
        plog("        +0x%02X = %08lX", k, v);
        dump_str_at("      str", v);
        if (readable(v)) {
            dump_str_at("      *ptr", rd(v));
            if (depth < 2) {
                DWORD w = rd(v);
                if (readable(w)) dump_str_at("      **ptr", rd(w));
            }
        }
    }
}

/* Every address holding a pointer to `v`. */
static int scan_ptr_to(DWORD v, DWORD *out, int max_out)
{
    MEMORY_BASIC_INFORMATION mbi;
    unsigned char *addr = NULL;
    int n = 0;
    while (VirtualQuery(addr, &mbi, sizeof mbi) == sizeof mbi) {
        unsigned char *next = (unsigned char *)mbi.BaseAddress + mbi.RegionSize;
        int own = g_own_stack &&
                  (unsigned char *)g_own_stack >= (unsigned char *)mbi.BaseAddress &&
                  (unsigned char *)g_own_stack < next;
        if (!own && mbi.State == MEM_COMMIT &&
            !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
            (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE))) {
            unsigned char *p = (unsigned char *)mbi.BaseAddress, *e = next - 4;
            for (; p <= e; p += 4)
                if (*(DWORD *)p == v) { if (n < max_out) out[n] = (DWORD)(uintptr_t)p; n++; }
        }
        if (next <= addr) break;
        addr = next;
    }
    return n;
}

/*
 * The console processor lives in slot +0x1F8 of the input-processor manager
 * (0x00489C63). Find the manager by looking for whatever points at a live
 * console processor, then locate the quick-access processor in the same object
 * -- that one is demonstrably active, so any difference between the two slots,
 * or any field pointing at one and not the other, is the activation mechanism.
 */
static void find_manager(void)
{
    DWORD ci[8], qa[8], holders[16];
    int nc = scan_vtable(VT_INPUT_CONSOLE, ci, 8);
    int nq = scan_vtable(VT_QUICKACCESS, qa, 8);
    int i, j, k, nh;

    plog("");
    plog("=== input-processor manager ===");
    if (!nc || !nq) { plog("  need both processors live; console=%d quick=%d", nc, nq); return; }
    plog("  console proc  0x%08lX", ci[0]);
    plog("  quickaccess   0x%08lX", qa[0]);

    nh = scan_ptr_to(ci[0], holders, 16);
    plog("  %d location(s) point at the console processor", nh);

    for (i = 0; i < nh && i < 8; i++) {
        DWORD h = holders[i], base;
        int found_qa = 0;
        /* the slot is +0x1F8, so the manager starts here */
        base = h - 0x1F8;
        plog("");
        plog("  holder 0x%08lX -> manager would be 0x%08lX", h, base);
        for (k = 0; k < 0x400; k += 4) {
            DWORD v = rd(base + k);
            for (j = 0; j < nq; j++)
                if (v == qa[j]) {
                    plog("    +0x%03X = quickaccess proc 0x%08lX", k, v);
                    found_qa = 1;
                }
            for (j = 0; j < nc; j++)
                if (v == ci[j]) plog("    +0x%03X = console proc     0x%08lX", k, v);
        }
        if (!found_qa) { plog("    (no quick-access processor nearby -- not the manager)"); continue; }
        plog("    --- manager 0x%08lX, bytes +0x1E0..+0x220 ---", base);
        for (k = 0x1E0; k < 0x220; k += 4)
            plog("      +0x%03X = %08lX", k, rd(base + k));
    }
}

static DWORD WINAPI probe_main(LPVOID unused)
{
    int t;
    int marker;
    (void)unused;
    g_own_stack = &marker;

    g_log = fopen(LOG_PATH, "w");
    if (!g_log) return 0;
    plog("=== console: is the dev console instantiated in retail? ===");
    plog("read-only: scanning for console vtables, no calls, no patches");

    /* Do NOT wait for the game object graph. The console is built during
     * startup ("Init Global Console"), long before a save is loaded, and
     * waiting for gsi meant the scan never ran at the frontend at all. */
    Sleep(8000);
    plog("scanning at t=8s (gsi %s)", rd(GSI_PTR) ? "up" : "not up - frontend");

    /* The quick-access processor is the control: it is definitely live -- the
     * seal is used through it -- so if it scans as 0 the scan itself is wrong
     * rather than the console being absent. */
    report("CInputProcessQuickAccess", VT_QUICKACCESS);
    report("CConsole", VT_CONSOLE);
    deep_console();
    {
        DWORD hits[4];
        if (scan_vtable(VT_CONSOLE, hits, 4) && rd(hits[0]) == VT_CONSOLE) {
            plog("");
            plog("  --- walking CConsole containers ---");
            walk_container("+0x04", rd(hits[0] + 0x04), 0);
            walk_container("+0x10", rd(hits[0] + 0x10), 0);
            walk_container("+0x40", rd(hits[0] + 0x40), 0);
            walk_container("+0x4C", rd(hits[0] + 0x4C), 0);
        }
    }
    find_manager();
    report("CTBaseSingleton<CConsole>", VT_CONSOLE_SINGLETON);
    report("CInputProcessConsole", VT_INPUT_CONSOLE);
    report("CConsoleCommandLine", VT_COMMAND_LINE);

    for (t = 0; t < 10; t++) {
        Sleep(30000);
        if (rd(GSI_PTR)) {
            plog("");
            plog("--- gsi came up; rescanning in-game ---");
            report("CConsole", VT_CONSOLE);
            report("CInputProcessConsole", VT_INPUT_CONSOLE);
            break;
        }
    }
    plog("");
    plog("=== done ===");
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
