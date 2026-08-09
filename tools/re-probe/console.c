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
        if (mbi.State == MEM_COMMIT &&
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

static DWORD WINAPI probe_main(LPVOID unused)
{
    int t;
    (void)unused;

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
