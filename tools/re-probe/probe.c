/*
 * re-probe: a throwaway diagnostic DLL for iterating on Fable hooks from macOS.
 *
 * The real mod can only be built with MSVC (SLikeNet is a prebuilt MSVC C++
 * static lib), but a standalone probe has no such dependency and cross-compiles
 * cleanly with mingw-w64. That gives a build/inject/observe loop that runs
 * locally under Wine instead of round-tripping through Windows CI.
 *
 * This probe only reads memory and writes a log. It does not modify the game.
 */

#include <windows.h>
#include <stdio.h>

#define LOG_PATH "probe.log"

static FILE *g_log;

static void plog(const char *fmt, ...)
{
    va_list ap;
    if (!g_log)
        return;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

/* Dump `n` bytes at `va` as hex, guarded so a bad address logs instead of crashing. */
static void dump(const char *label, DWORD va, int n)
{
    unsigned char buf[32];
    char hex[32 * 3 + 1];
    int i;

    if (n > (int)sizeof buf)
        n = sizeof buf;

    if (IsBadReadPtr((const void *)(uintptr_t)va, n)) {
        plog("  %-34s 0x%08lX  <unreadable>", label, va);
        return;
    }
    memcpy(buf, (const void *)(uintptr_t)va, n);
    for (i = 0; i < n; i++)
        sprintf(hex + i * 3, "%02x ", buf[i]);
    plog("  %-34s 0x%08lX  %s", label, va, hex);
}

/* Verify the bytes at `va` match what static analysis of Fable.exe predicted. */
static int expect(const char *label, DWORD va, const unsigned char *want, int n)
{
    int ok;
    if (IsBadReadPtr((const void *)(uintptr_t)va, n)) {
        plog("  [FAIL] %-28s 0x%08lX unreadable", label, va);
        return 0;
    }
    ok = memcmp((const void *)(uintptr_t)va, want, n) == 0;
    plog("  [%s] %-28s 0x%08lX", ok ? " OK " : "FAIL", label, va);
    return ok;
}

static DWORD WINAPI probe_main(LPVOID unused)
{
    HMODULE base;

    (void)unused;

    g_log = fopen(LOG_PATH, "w");
    if (!g_log)
        return 0;

    plog("=== re-probe attached ===");

    base = GetModuleHandleA(NULL);
    plog("Fable.exe base   = 0x%08lX  (expected 0x00400000)", (DWORD)(uintptr_t)base);
    plog("probe module     = loaded");

    if ((DWORD)(uintptr_t)base != 0x00400000) {
        plog("!! base differs from static analysis - every hardcoded address below is invalid");
        fclose(g_log);
        g_log = NULL;
        return 0;
    }

    plog("");
    plog("--- verifying static analysis against the live image ---");

    /* sub_59899A prologue: push ebp ; mov ebp,esp ; sub esp,0x10 */
    {
        static const unsigned char want[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x10 };
        expect("sub_59899A prologue", 0x0059899A, want, sizeof want);
    }
    /* push 0x1252374  -> UI_FRONTEND_MAIN_MENU_NO_LIVEAWARE_NO_CONTINUE */
    {
        static const unsigned char want[] = { 0x68, 0x74, 0x23, 0x25, 0x01 };
        expect("push NO_CONTINUE def name", 0x005989E1, want, sizeof want);
    }
    /* push 0x12524E4  -> UI_FRONTEND_MAIN_MENU_NO_LIVEAWARE */
    {
        static const unsigned char want[] = { 0x68, 0xE4, 0x24, 0x25, 0x01 };
        expect("push MAIN_MENU def name", 0x005989E8, want, sizeof want);
    }
    /* CFrontEndManager ctor region */
    {
        static const unsigned char want[] = { 0x55 };
        expect("CFrontEndManager ctor", 0x00595356, want, sizeof want);
    }

    plog("");
    plog("--- definition name strings as the process sees them ---");
    plog("  0x01252374 = %s", (const char *)0x01252374);
    plog("  0x012524E4 = %s", (const char *)0x012524E4);
    plog("  0x01252878 = %s", (const char *)0x01252878);
    plog("  0x012528B0 = %s", (const char *)0x012528B0);

    plog("");
    plog("--- vtable pointers (should sit in .rdata, 0x122D000+) ---");
    dump("CFrontEndManager vtable", 0x012521A8, 12);
    dump("CFrontEndScreen vtable", 0x012497E4, 12);
    dump("CFrontEndButton vtable", 0x01249554, 12);

    plog("");
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
        /* Do the work off the loader lock. */
        CloseHandle(CreateThread(NULL, 0, probe_main, NULL, 0, NULL));
    }
    return TRUE;
}
