/*
 * re-probe: maps the UI definition structure that Fable's component factory
 * consumes.
 *
 * Established previously (docs/ui-system.md): the factory at 0x0041D21B reads
 * the component type from def+0x3C and dispatches through a jump table of 44
 * types. Mapping the rest of that definition is what stands between reusing
 * existing definitions and authoring arbitrary new UI.
 *
 * Strategy: a live component must reference the definition it was built from,
 * so search each component's fields for a pointer to an object whose +0x3C
 * holds that component's own type id. That finds both the def-pointer offset
 * and a real definition, without guessing either.
 *
 * Read-only: this probe only reads memory and writes a log.
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>

#define LOG_PATH "probe.log"

/* Component vtables (RTTI-derived, verified live). */
#define VT_FRONTEND_LIST   0x01249224u
#define VT_TEXT            0x01249CCCu

#define VEC_BEGIN 0x164
#define VEC_END   0x168
#define VEC_CAP   0x16C

#define TYPE_OFF 0x3C                 /* component type inside a definition */
#define TYPE_FRONTEND_BUTTON 0x0B
#define TYPE_TEXT            0x06

/* .rdata, where vtables live -- used to sanity-check a candidate definition. */
#define RDATA_LO 0x0122D000u
#define RDATA_HI 0x01374000u

#define DEF_DUMP 0x140
#define MAX_OBJ  64

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
    return mbi->State == MEM_COMMIT && mbi->Type == MEM_PRIVATE &&
           !(mbi->Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
           (mbi->Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE));
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

static const char *comp_defname(DWORD c)
{
    DWORD holder, str;
    if (IsBadReadPtr((void *)(uintptr_t)(c + 0x20), 4)) return NULL;
    holder = *(DWORD *)(uintptr_t)(c + 0x20);
    if (holder < 0x10000 || IsBadReadPtr((void *)(uintptr_t)holder, 4)) return NULL;
    str = *(DWORD *)(uintptr_t)holder;
    if (str < 0x10000 || IsBadReadPtr((void *)(uintptr_t)str, 8)) return NULL;
    return (const char *)(uintptr_t)str;
}

/* The member of `comp` pointing at a definition whose +0x3C == want. */
static DWORD find_def(DWORD comp, DWORD want, int *out_off)
{
    int off;
    for (off = 0; off < 0x200; off += 4) {
        DWORD p, vt;
        if (IsBadReadPtr((void *)(uintptr_t)(comp + (DWORD)off), 4)) continue;
        p = *(DWORD *)(uintptr_t)(comp + (DWORD)off);
        if (p < 0x10000) continue;
        if (IsBadReadPtr((void *)(uintptr_t)(p + TYPE_OFF), 4)) continue;
        if (*(DWORD *)(uintptr_t)(p + TYPE_OFF) != want) continue;
        vt = *(DWORD *)(uintptr_t)p;               /* defs start with a vtable */
        if (vt < RDATA_LO || vt >= RDATA_HI) continue;
        if (out_off) *out_off = off;
        return p;
    }
    return 0;
}

static int try_str(DWORD va, char *out, int cap)
{
    const unsigned char *p = (const unsigned char *)(uintptr_t)va;
    int i;
    if (va < 0x10000 || IsBadReadPtr(p, 4)) return 0;
    for (i = 0; i < cap - 1; i++) {
        if (IsBadReadPtr(p + i, 1)) return 0;
        if (p[i] == 0) break;
        if (p[i] < 0x20 || p[i] > 0x7E) return 0;
        out[i] = (char)p[i];
    }
    out[i] = 0;
    return i >= 3;
}


/* Describe a raw dword. Order matters: a heap pointer reinterpreted as a float
 * is a denormal that prints as "0.000", which hid the real fields on the first
 * pass. Strings and pointers are therefore ruled out before floats. */
static void fmt_val(DWORD v, char *out, int cap)
{
    float f = *(float *)&v;
    char s[64];
    if (v == 0) { snprintf(out, cap, "0"); return; }
    if (try_str(v, s, sizeof s)) { snprintf(out, cap, "\"%s\"", s); return; }
    if (v >= 0x00400000u && v < 0x10000000u &&
        !IsBadReadPtr((void *)(uintptr_t)v, 4)) { snprintf(out, cap, "ptr"); return; }
    if (v < 0x10000u) { snprintf(out, cap, "%lu", v); return; }
    if ((f > 0.0009f && f < 100000.0f) || (f < -0.0009f && f > -100000.0f)) {
        snprintf(out, cap, "%.2ff", (double)f); return;
    }
    snprintf(out, cap, "-");
}


/* Dump the vector at def+0x40..+0x48. It is not empty (begin != end) and
 * begin+size == capacity, so it is a packed array -- the natural home for the
 * per-component properties that the scalar diff does not account for. */
static void dump_def_vector(const char *label, DWORD def)
{
    DWORD begin, end, cap, bytes, i;
    if (IsBadReadPtr((void *)(uintptr_t)(def + 0x48), 4)) return;
    begin = *(DWORD *)(uintptr_t)(def + 0x40);
    end   = *(DWORD *)(uintptr_t)(def + 0x44);
    cap   = *(DWORD *)(uintptr_t)(def + 0x48);
    if (begin < 0x10000 || end <= begin) {
        plog("  %s vector: begin=0x%08lX end=0x%08lX (empty)", label, begin, end);
        return;
    }
    bytes = end - begin;
    plog("");
    plog("  %s vector @ 0x%08lX  %lu bytes  (end=0x%08lX cap=0x%08lX)",
         label, begin, bytes, end, cap);
    if (bytes > 0x400) bytes = 0x400;

    for (i = 0; i + 4 <= bytes; i += 4) {
        DWORD v;
        char d[80];
        if (IsBadReadPtr((void *)(uintptr_t)(begin + i), 4)) break;
        v = *(DWORD *)(uintptr_t)(begin + i);
        fmt_val(v, d, sizeof d);
        /* if it points at something, try to read a string one hop in */
        if (v >= 0x00400000u && v < 0x10000000u &&
            !IsBadReadPtr((void *)(uintptr_t)v, 4)) {
            char s2[64];
            DWORD inner = *(DWORD *)(uintptr_t)v;
            if (try_str(inner, s2, sizeof s2))
                plog("    [%3lu] +0x%03lX  %08lX -> \"%s\"", i / 4, i, v, s2);
            else if (try_str(v, s2, sizeof s2))
                plog("    [%3lu] +0x%03lX  %08lX  \"%s\"", i / 4, i, v, s2);
            else
                plog("    [%3lu] +0x%03lX  %08lX  %s", i / 4, i, v, d);
        } else {
            plog("    [%3lu] +0x%03lX  %08lX  %s", i / 4, i, v, d);
        }
    }
}


#define UISTATE_SIZE 0x7C   /* sizeof(CUIStateDef), vtable 0x0125871C */

/* Diff state[0] of two definitions of the same type. The per-state record is
 * where the visuals live -- colour, scale, and the banked asset ids. */
static void diff_states(DWORD d1, DWORD d2, unsigned long ty)
{
    DWORD b1, b2, e1, e2;
    int off, ndiff = 0;
    if (IsBadReadPtr((void *)(uintptr_t)(d1 + 0x48), 4)) return;
    if (IsBadReadPtr((void *)(uintptr_t)(d2 + 0x48), 4)) return;
    b1 = *(DWORD *)(uintptr_t)(d1 + 0x40);
    e1 = *(DWORD *)(uintptr_t)(d1 + 0x44);
    b2 = *(DWORD *)(uintptr_t)(d2 + 0x40);
    e2 = *(DWORD *)(uintptr_t)(d2 + 0x44);
    if (b1 < 0x10000 || b2 < 0x10000 || e1 <= b1 || e2 <= b2) return;

    plog("");
    plog("  === type 0x%02lX CUIStateDef[0] diff (%lu vs %lu states) ===",
         ty, (e1 - b1) / UISTATE_SIZE, (e2 - b2) / UISTATE_SIZE);
    for (off = 0; off < UISTATE_SIZE; off += 4) {
        DWORD va, vb;
        char sa[80], sb[80];
        if (IsBadReadPtr((void *)(uintptr_t)(b1 + (DWORD)off), 4)) break;
        if (IsBadReadPtr((void *)(uintptr_t)(b2 + (DWORD)off), 4)) break;
        va = *(DWORD *)(uintptr_t)(b1 + (DWORD)off);
        vb = *(DWORD *)(uintptr_t)(b2 + (DWORD)off);
        if (va == vb) continue;
        ndiff++;
        fmt_val(va, sa, sizeof sa);
        fmt_val(vb, sb, sizeof sb);
        plog("    +0x%03X  %08lX %-12s |  %08lX %s", off, va, sa, vb, sb);
    }
    plog("    (%d differing fields of %d)", ndiff, UISTATE_SIZE / 4);
}

static void dump_def(const char *label, DWORD def, int size)
{
    int off;
    plog("");
    plog("  --- %s : definition @ 0x%08lX (vtable 0x%08lX) ---",
         label, def, *(DWORD *)(uintptr_t)def);
    for (off = 0; off < size; off += 4) {
        DWORD v;
        float f;
        char buf[80];
        if (IsBadReadPtr((void *)(uintptr_t)(def + (DWORD)off), 4)) continue;
        v = *(DWORD *)(uintptr_t)(def + (DWORD)off);
        f = *(float *)&v;
        if (off == TYPE_OFF) {
            plog("    +0x%03X  0x%08lX   <== COMPONENT TYPE (%lu)", off, v, v);
        } else if (try_str(v, buf, sizeof buf)) {
            plog("    +0x%03X  0x%08lX   str  \"%s\"", off, v, buf);
        } else if (v && !IsBadReadPtr((void *)(uintptr_t)v, 4) &&
                   try_str(*(DWORD *)(uintptr_t)v, buf, sizeof buf)) {
            plog("    +0x%03X  0x%08lX   ->   \"%s\"", off, v, buf);
        } else if (f > -10000.0f && f < 10000.0f && f != 0.0f &&
                   (v & 0x7F800000) != 0 && (v & 0x7F800000) != 0x7F800000) {
            plog("    +0x%03X  0x%08lX   float %.3f", off, v, (double)f);
        } else if (v < 0x10000) {
            plog("    +0x%03X  0x%08lX   int  %lu", off, v, v);
        } else {
            plog("    +0x%03X  0x%08lX   ptr", off, v);
        }
    }
}


/*
 * Calling the game's own functions.
 *
 * The factory path revealed a definition-lookup API:
 *     mgr = CGameDefinitionManager::Get()      0x0044C6B0, no args, result in eax
 *     def = mgr->GetDefinition(&CharString, 1) 0x009AD390, thiscall
 * with CharString built by its ctor/dtor pair:
 *     CharString::CharString(const char *s, int len)  0x0099EBF0  thiscall
 *     CharString::~CharString()                       0x0099EAE0  thiscall
 *
 * These are thiscall (this in ecx, callee cleans the stack), so they are
 * invoked through inline asm rather than a C prototype.
 */
#define FN_DEFMGR_GET   0x0044C6B0u
#define FN_GET_DEF      0x009AD390u
#define FN_CHARSTR_CTOR 0x0099EBF0u
#define FN_CHARSTR_DTOR 0x0099EAE0u

static DWORD call0(DWORD fn)
{
    DWORD ret;
    __asm__ __volatile__("call *%[f]" : "=a"(ret) : [f]"r"(fn)
                         : "ecx", "edx", "memory", "cc");
    return ret;
}

static DWORD call_this0(DWORD fn, DWORD thisp)
{
    DWORD ret;
    __asm__ __volatile__("movl %[t], %%ecx\n\tcall *%[f]"
                         : "=a"(ret) : [f]"r"(fn), [t]"r"(thisp)
                         : "ecx", "edx", "memory", "cc");
    return ret;
}

/* Stack ends up [esp]=a1, [esp+4]=a2, matching the game's own push order. */
static DWORD call_this2(DWORD fn, DWORD thisp, DWORD a1, DWORD a2)
{
    DWORD ret;
    __asm__ __volatile__(
        "pushl %[b]\n\t"
        "pushl %[a]\n\t"
        "movl %[t], %%ecx\n\t"
        "call *%[f]"
        : "=a"(ret)
        : [f]"r"(fn), [t]"r"(thisp), [a]"r"(a1), [b]"r"(a2)
        : "ecx", "edx", "memory", "cc");
    return ret;
}

/*
 * The definition manager.
 *
 * 0x0044C6B0 is simply `mov eax,[0x013B879C]; ret` -- a global pointer. The
 * factory uses the container's own manager at [obj+0x64] when set, and only
 * falls back to this global, so both are tried here.
 *
 * 0x009AD390 is GetDefinition(nameKey, index): it calls the name lookup at
 * 0x009AD2E0 and then indexes [result+8] by `index`.
 */
#define G_DEFMGR    0x013B879Cu
#define FN_NAME_LOOKUP 0x009AD2E0u

static DWORD g_defmgr;

static DWORD get_def_by_name(const char *name)
{
    unsigned char str[128];
    DWORD def;
    if (!g_defmgr) return 0;
    memset(str, 0, sizeof str);
    call_this2(FN_CHARSTR_CTOR, (DWORD)(uintptr_t)str,
               (DWORD)(uintptr_t)name, (DWORD)-1);
    def = call_this2(FN_GET_DEF, g_defmgr, (DWORD)(uintptr_t)str, 1);
    call_this0(FN_CHARSTR_DTOR, (DWORD)(uintptr_t)str);
    return def;
}


/*
 * Capturing a real definition pointer.
 *
 * The definition-manager global is NULL from our thread, so instead we catch
 * the factory in the act. At 0x0041D249 the factory does `mov eax,[ebx+0x3C]`
 * with EBX already holding the definition, so a one-shot INT3 there plus a
 * vectored handler hands us a genuine def pointer with its type.
 *
 * INT3 is an ordinary breakpoint exception; Wine delivers it (unlike the guard
 * pages tried earlier, which never fired).
 */
#define FACTORY_TYPE_READ 0x0041D249u

#define MAX_CAUGHT 64
static unsigned char g_orig_byte;
static volatile DWORD g_caught[MAX_CAUGHT];
static volatile LONG  g_caught_n;

static LONG CALLBACK bp_handler(EXCEPTION_POINTERS *ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_BREAKPOINT &&
        (DWORD)(uintptr_t)ep->ExceptionRecord->ExceptionAddress == FACTORY_TYPE_READ) {
        DWORD prot;
        DWORD d = ep->ContextRecord->Ebx;
        LONG i;
        int seen = 0;
        /* EBX holds the definition the factory is about to read the type from. */
        for (i = 0; i < g_caught_n; i++)
            if (g_caught[i] == d) { seen = 1; break; }
        if (!seen && g_caught_n < MAX_CAUGHT)
            g_caught[g_caught_n++] = d;
        /* Restore the byte and re-run the real instruction; the probe re-arms. */
        if (VirtualProtect((void *)(uintptr_t)FACTORY_TYPE_READ, 1,
                           PAGE_EXECUTE_READWRITE, &prot)) {
            *(unsigned char *)(uintptr_t)FACTORY_TYPE_READ = g_orig_byte;
            VirtualProtect((void *)(uintptr_t)FACTORY_TYPE_READ, 1, prot, &prot);
        }
        ep->ContextRecord->Eip = FACTORY_TYPE_READ;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static int g_orig_saved;

static int arm_factory_bp(void)
{
    DWORD prot;
    unsigned char cur = *(unsigned char *)(uintptr_t)FACTORY_TYPE_READ;
    if (cur == 0xCC) return 1;                 /* already armed */
    if (!g_orig_saved) { g_orig_byte = cur; g_orig_saved = 1; }
    if (!VirtualProtect((void *)(uintptr_t)FACTORY_TYPE_READ, 1,
                        PAGE_EXECUTE_READWRITE, &prot))
        return 0;
    *(unsigned char *)(uintptr_t)FACTORY_TYPE_READ = 0xCC;
    VirtualProtect((void *)(uintptr_t)FACTORY_TYPE_READ, 1, prot, &prot);
    return 1;
}

static DWORD WINAPI probe_main(LPVOID unused)
{
    DWORD lists[MAX_OBJ], texts[MAX_OBJ];
    int nl, i, t, marker;
    DWORD begin = 0, count = 0, menu = 0;
    DWORD btn_a = 0, btn_b = 0;

    (void)unused;
    g_own_stack = &marker;
    g_log = fopen(LOG_PATH, "w");
    if (!g_log) return 0;

    plog("=== re-probe: map the UI definition structure ===");

    AddVectoredExceptionHandler(1, bp_handler);
    plog("factory breakpoint at 0x%08X: %s", FACTORY_TYPE_READ,
         arm_factory_bp() ? "armed" : "FAILED");

    /* Re-arm tightly: components are constructed in a burst, and each hit
     * disarms the breakpoint, so a slow cadence samples almost nothing. */
    for (t = 0; t < 60000 && g_caught_n < MAX_CAUGHT; t++) {
        arm_factory_bp();
        Sleep(1);
    }
    plog("tight re-arm finished, %ld definitions captured", g_caught_n);

    for (t = 0; t < 30 && !menu; t++) {
        Sleep(2000);
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
                const char *nm = comp_defname(*(DWORD *)(uintptr_t)(b + k * 4));
                if (nm && strcmp(nm, "UI_FRONTEND_BUTTON_QUIT") == 0) {
                    menu = lists[i]; begin = b; count = n;
                }
            }
        }
    }
    if (!menu) plog("(main menu not located this run -- capture is independent of it)");

    if (menu) {
        plog("main menu list 0x%08lX, %lu children", menu, count);
        btn_a = *(DWORD *)(uintptr_t)(begin + 0);
        btn_b = *(DWORD *)(uintptr_t)(begin + (count - 1) * 4);
        plog("  A 0x%08lX  %s", btn_a, comp_defname(btn_a));
        plog("  B 0x%08lX  %s", btn_b, comp_defname(btn_b));
    }

    /* Fetch definitions by name through the game's own API. */
    {
        static const char *names[] = {
            "UI_FRONTEND_BUTTON_QUIT",
            "UI_FRONTEND_BUTTON_CREDITS",
            "UI_FRONTEND_MAIN_MENU",
            "UI_FRONTEND_BUTTON_LIVE_AWARE",
        };
        DWORD defs[4];
        unsigned k;

        plog("");
        plog("=== definitions captured from the factory ===");
        plog("  distinct definitions: %ld", g_caught_n);
        {
            LONG i;
            DWORD by_type[0x2C];
            memset(by_type, 0, sizeof by_type);
            for (i = 0; i < g_caught_n; i++) {
                DWORD d = g_caught[i], ty;
                if (IsBadReadPtr((void *)(uintptr_t)(d + TYPE_OFF), 4)) continue;
                ty = *(DWORD *)(uintptr_t)(d + TYPE_OFF);
                plog("    [%2ld] 0x%08lX  type=%2lu", i, d, ty);
                if (ty < 0x2C && !by_type[ty]) by_type[ty] = d;
            }
            /* Dump one definition per distinct type, then diff two of a kind. */
            for (i = 0; i < 0x2C; i++) {
                char lbl[32];
                if (!by_type[i]) continue;
                sprintf(lbl, "type 0x%02lX", (unsigned long)i);
                dump_def(lbl, by_type[i], DEF_DUMP);
                dump_def_vector(lbl, by_type[i]);
            }
            {
                /* Diff every type with two or more captured definitions: fields
                 * that differ between two definitions of the SAME type are the
                 * per-instance content (position, size, asset ids) rather than
                 * type machinery. */
                DWORD ty;
                for (ty = 0; ty < 0x2C; ty++) {
                    DWORD d1 = 0, d2 = 0;
                    LONG j;
                    int off, ndiff = 0;
                    for (j = 0; j < g_caught_n; j++) {
                        DWORD d = g_caught[j];
                        if (IsBadReadPtr((void *)(uintptr_t)(d + TYPE_OFF), 4)) continue;
                        if (*(DWORD *)(uintptr_t)(d + TYPE_OFF) != ty) continue;
                        if (!d1) d1 = d; else if (!d2) { d2 = d; break; }
                    }
                    if (!d1 || !d2) continue;

                    plog("");
                    plog("  === type 0x%02lX : diff 0x%08lX vs 0x%08lX ===", ty, d1, d2);
                    for (off = 0; off < DEF_DUMP; off += 4) {
                        DWORD va, vb;
                        float fa, fb;
                        char sa[64], sb[64];
                        if (IsBadReadPtr((void *)(uintptr_t)(d1 + (DWORD)off), 4)) continue;
                        if (IsBadReadPtr((void *)(uintptr_t)(d2 + (DWORD)off), 4)) continue;
                        va = *(DWORD *)(uintptr_t)(d1 + (DWORD)off);
                        vb = *(DWORD *)(uintptr_t)(d2 + (DWORD)off);
                        if (va == vb) continue;
                        ndiff++;
                        (void)fa; (void)fb;
                        fmt_val(va, sa, sizeof sa);
                        fmt_val(vb, sb, sizeof sb);
                        plog("    +0x%03X  %08lX %-12s |  %08lX %s",
                             off, va, sa, vb, sb);
                    }
                    plog("    (%d differing fields)", ndiff);
                    diff_states(d1, d2, ty);
                }
            }
        }
    }

done:
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
        CloseHandle(CreateThread(NULL, 0, probe_main, NULL, 0, NULL));
    }
    return TRUE;
}
