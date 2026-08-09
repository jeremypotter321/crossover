/*
 * console-vocab: check the statically-recovered console table against the
 * running game.
 *
 * `tools/re-static/console-cmds.py` reads the whole console vocabulary out of
 * Fable.exe -- 20 commands and 22 variables, with each command's handler and
 * each variable's storage address. This confirms that reading against the live
 * process and answers the two questions the static pass cannot:
 *
 *   1. Is the table complete? Anything registered at run time (rather than by
 *      the inline idiom in .text) would show up here and not there.
 *   2. Are the ini files really executed as console scripts? `userst.ini` sets
 *      nine of the variables, so if the live bytes match the file, the console
 *      ran it. `CConsole::RunScript` is handed "userst.ini" at 0x00414C7F and
 *      "user.ini" at 0x00418981, which is the static half of that claim.
 *
 * READ-ONLY. No call into game code, no patched bytes, no breakpoints -- every
 * crash this project has caused came from one of those on the game's thread,
 * and none of them are needed here.
 */

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>

#define LOG_PATH "C:\\Games\\Fable\\console-vocab.log"

#define CONSOLE_SINGLETON 0x013CAA40u  /* holds CConsole*; ctor 0x009ECD80 */
#define VT_CONSOLE        0x0129C600u

/* vtable -> class, for whatever the scan turns up. */
#define VT_CLASSLESS      0x0122E65Cu
#define VT_CMD_CONSOLE    0x0129C4E0u
#define VT_CMD_TIMEMGR    0x0125D668u
#define VT_VARIABLE       0x0122E5C8u

struct var_ent { const char *name; DWORD addr; int bytes; const char *ini; };

/* From console-cmds.py. `ini` is what userst.ini asks for, or NULL if the file
 * never mentions it. The four-byte ones are pool sizes; no ini sets them. */
static const struct var_ent g_vars[] = {
    { "UsePhysicalDVD",              0x013B85F2u, 1, "TRUE"  },
    { "UseCompiledGlobalThings",     0x013B8609u, 1, "TRUE"  },
    { "UseCompiledAnimationEvents",  0x013B860Au, 1, "TRUE"  },
    { "RunFromDVD",                  0x013B8615u, 1, "TRUE"  },
    { "UseRetailBanks",              0x013B8616u, 1, "TRUE"  },
    { "UseCompiledDefs",             0x013B8617u, 1, "TRUE"  },
    { "UseCompiledWorldFiles",       0x013B8618u, 1, "TRUE"  },
    { "UseCompiledSoundSymbols",     0x013B8619u, 1, "TRUE"  },
    { "UseRetailSaveGameSystem",     0x013B8646u, 1, NULL    },
    { "AllowDataGeneration",         0x01375459u, 1, "FALSE" },
    { "InstallerBufferSize",         0x01375494u, 4, NULL    },
    { "NoInstallBuffers",            0x01375498u, 4, NULL    },
    { "LandscapePoolSize",           0x0137549Cu, 4, NULL    },
    { "LandscapePhysicalMemoryRatio",0x013754A0u, 4, NULL    },
    { "LandscapeVirtualMemoryMinSize",0x013754A4u,4, NULL    },
    { "MeshPoolSize",                0x013754A8u, 4, NULL    },
    { "MeshStatsPoolSize",           0x013754ACu, 4, NULL    },
    { "MeshPhysicalMemoryRatio",     0x013754B0u, 4, NULL    },
    { "HiresTextureMemory",          0x013754B4u, 4, NULL    },
    { "PhysicsMeshPoolSize",         0x013754B8u, 4, NULL    },
    { "AnimationPoolSize",           0x013754BCu, 4, NULL    },
    { "ClothPoolSize",               0x013754C0u, 4, NULL    },
};
#define NVARS (int)(sizeof g_vars / sizeof g_vars[0])

/* Derived flags: AllowDataGeneration is copied into these at 0x004025DA, and
 * 0x0138E188 is what makes the definition manager WRITE a compiled .bin
 * instead of only reading one (0x009B0A26). That is the Route B lever. */
static const struct var_ent g_derived[] = {
    { "(gen) data-write flag",   0x0138E188u, 1, NULL },
    { "(gen) def-write flag",    0x0138E189u, 1, NULL },
    { "(gen) third copy",        0x0138DD3Bu, 1, NULL },
    { "UseCompiledDefs runtime", 0x013CA7D8u, 1, NULL },
    { "userst.ini gate",         0x01375444u, 1, NULL },
};
#define NDERIVED (int)(sizeof g_derived / sizeof g_derived[0])

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
static DWORD rd(DWORD p)  { return readable(p) ? *(DWORD *)(uintptr_t)p : 0; }
static int   rb(DWORD p)  { return readable(p) ? *(unsigned char *)(uintptr_t)p : -1; }

/* Printable NUL-terminated ASCII at `p`, or NULL. */
static const char *str_at(DWORD p)
{
    int k;
    if (!readable(p)) return NULL;
    for (k = 0; k < 64; k++) {
        int c = rb(p + k);
        if (c == 0) return k >= 2 ? (const char *)(uintptr_t)p : NULL;
        if (c < 0x20 || c > 0x7E) return NULL;
    }
    return NULL;
}

/*
 * The name of a console entry. Its CharString is embedded at +0x04, but which
 * indirection reaches the characters is not yet established -- def-map.c's
 * component names go through two derefs, and a first pass assuming one deref
 * decoded nothing. So try the plausible shapes and report which one hit, rather
 * than guessing again.
 */
static const char *entry_name(DWORD obj, const char **how)
{
    const char *s;
    if ((s = str_at(obj + 4)))            { *how = "inline +0x04";  return s; }
    if ((s = str_at(rd(obj + 4))))        { *how = "*+0x04";        return s; }
    if ((s = str_at(rd(rd(obj + 4)))))    { *how = "**+0x04";       return s; }
    if ((s = str_at(rd(obj + 8))))        { *how = "*+0x08";        return s; }
    *how = NULL;
    return NULL;
}

static const char *class_of(DWORD vt)
{
    switch (vt) {
    case VT_CLASSLESS:   return "ClasslessCommand";
    case VT_CMD_CONSOLE: return "Command<CConsole>";
    case VT_CMD_TIMEMGR: return "Command<CGameTimeManager>";
    case VT_VARIABLE:    return "Variable";
    default:             return NULL;
    }
}

/*
 * Enumerate live entries by scanning for the four class vtables. The registry
 * container at CConsole+0x40 is an MSVC map and walking one blind has cost this
 * project time before; a vtable scan is the shape that has always worked here,
 * and it also catches anything the container does not hold.
 */
static void enumerate(void)
{
    static const DWORD vts[] = { VT_CLASSLESS, VT_CMD_CONSOLE,
                                 VT_CMD_TIMEMGR, VT_VARIABLE };
    MEMORY_BASIC_INFORMATION mbi;
    unsigned char *addr = NULL;
    int total = 0;

    plog("");
    plog("=== live console entries (vtable scan) ===");
    while (VirtualQuery(addr, &mbi, sizeof mbi) == sizeof mbi) {
        unsigned char *next = (unsigned char *)mbi.BaseAddress + mbi.RegionSize;
        int own = g_own_stack &&
                  (unsigned char *)g_own_stack >= (unsigned char *)mbi.BaseAddress &&
                  (unsigned char *)g_own_stack < next;
        if (!own && mbi.State == MEM_COMMIT &&
            !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
            (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE))) {
            unsigned char *p = (unsigned char *)mbi.BaseAddress;
            unsigned char *e = next - 0x20;
            for (; p <= e; p += 4) {
                DWORD v = *(DWORD *)p, obj = (DWORD)(uintptr_t)p;
                const char *cls, *nm, *how;
                unsigned i;
                for (i = 0; i < sizeof vts / sizeof vts[0]; i++)
                    if (v == vts[i]) break;
                if (i == sizeof vts / sizeof vts[0]) continue;
                cls = class_of(v);
                nm = entry_name(obj, &how);
                total++;
                plog("  @0x%08lX %-24s payload 0x%08lX  name %s (%s)", obj, cls,
                     rd(obj + (v == VT_VARIABLE ? 0x0C : v == VT_CLASSLESS ? 0x14 : 0x18)),
                     nm ? nm : "?", how ? how : "no decode");
                plog("      %08lX %08lX %08lX %08lX %08lX %08lX %08lX",
                     rd(obj), rd(obj + 4), rd(obj + 8), rd(obj + 0x0C),
                     rd(obj + 0x10), rd(obj + 0x14), rd(obj + 0x18));
            }
        }
        if (next <= addr) break;
        addr = next;
    }
    plog("  -- %d live entries (static table has 42) --", total);
}

static void dump_vars(const char *title, const struct var_ent *v, int n)
{
    int i;
    plog("");
    plog("=== %s ===", title);
    for (i = 0; i < n; i++) {
        if (!readable(v[i].addr)) {
            plog("  %-30s 0x%08lX  <unreadable>", v[i].name, v[i].addr);
            continue;
        }
        if (v[i].bytes == 1) {
            int b = rb(v[i].addr);
            const char *want = v[i].ini;
            const char *verdict = "";
            if (want)
                verdict = ((b != 0) == (want[0] == 'T')) ? "  MATCHES ini"
                                                         : "  *** DIFFERS from ini ***";
            plog("  %-30s 0x%08lX = %d%s%s%s", v[i].name, v[i].addr, b,
                 want ? "   ini says " : "", want ? want : "", verdict);
        } else {
            plog("  %-30s 0x%08lX = %lu (0x%08lX)", v[i].name, v[i].addr,
                 rd(v[i].addr), rd(v[i].addr));
        }
    }
}

static DWORD WINAPI probe_main(LPVOID unused)
{
    int marker;
    DWORD c;
    (void)unused;
    g_own_stack = &marker;

    g_log = fopen(LOG_PATH, "w");
    if (!g_log) return 0;
    plog("=== console-vocab: static table vs the running game ===");
    plog("read-only: no calls, no patches, no breakpoints");

    Sleep(4000);

    c = rd(CONSOLE_SINGLETON);
    plog("");
    plog("CConsole singleton *0x%08X = 0x%08lX  (vtable 0x%08lX, %s)",
         CONSOLE_SINGLETON, c, rd(c),
         rd(c) == VT_CONSOLE ? "correct" : "NOT CConsole");
    if (c)
        plog("  registry container +0x40 = 0x%08lX, count +0x44 = %lu",
             rd(c + 0x40), rd(c + 0x44));

    dump_vars("console variables vs userst.ini", g_vars, NVARS);
    dump_vars("flags derived from AllowDataGeneration", g_derived, NDERIVED);
    enumerate();

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
