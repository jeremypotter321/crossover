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
static void dump_vec_at(const char *label, DWORD def, int base, int elem);

static void dump_def_vector(const char *label, DWORD def)
{
    DWORD begin, end, cap, bytes, i;
    /* CUIDef holds a second vector at +0x70..+0x78. Screens build their own
     * children during init, so a child-definition list in the definition is
     * the only thing that could drive which children appear. */
    dump_vec_at(label, def, 0x70, 4);
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

static void dump_vec_at(const char *label, DWORD def, int base, int elem)
{
    DWORD b, e, n, i;
    if (IsBadReadPtr((void *)(uintptr_t)(def + (DWORD)base + 8), 4)) return;
    b = *(DWORD *)(uintptr_t)(def + (DWORD)base);
    e = *(DWORD *)(uintptr_t)(def + (DWORD)base + 4);
    if (b < 0x10000 || e <= b || (e - b) > 0x800) return;
    n = (e - b) / (DWORD)elem;
    plog("");
    plog("  %s vector@+0x%02X: %lu element(s) of %d bytes  [0x%08lX..0x%08lX]",
         label, base, n, elem, b, e);
    for (i = 0; i < n && i < 32; i++) {
        DWORD v = *(DWORD *)(uintptr_t)(b + i * (DWORD)elem);
        char d[80];
        fmt_val(v, d, sizeof d);
        plog("    [%2lu] %08lX  %s", i, v, d);
    }
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
static DWORD g_child_vec;

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
#define FACTORY_ENTRY     0x0041D21Bu

#define MAX_CAUGHT 64
static unsigned char g_orig_byte;
static volatile DWORD g_caught[MAX_CAUGHT];
static volatile LONG  g_caught_n;
static volatile LONG  g_patched_screens;
static volatile DWORD g_text_def;      /* a CText definition we reuse   */
static volatile DWORD g_text_id;       /* its id, for screen child lists */
static volatile LONG  g_text_injected;
static volatile LONG  g_hook_hits;

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

        /* HELLO WORLD.
         *
         * Reuse a real CText definition so the component is built by the game
         * with the game's own font, move it to the right via the definition's
         * own x/y (CUIDef+0x58/+0x5C) -- which avoids needing the component's
         * runtime X field, still unidentified -- and append its id to every
         * screen's child list so the main menu picks it up. */
        if (!IsBadReadPtr((void *)(uintptr_t)(d + 0x5C), 4) &&
            *(DWORD *)(uintptr_t)(d + TYPE_OFF) == 0x06 &&
            0) {   /* id is pinned above; no per-run picking */
            /* Lowest id wins, so the SAME definition is injected every run --
             * "first resolved" varied per launch and kept changing the text. */
            g_text_def = d;
            g_text_id  = *(DWORD *)(uintptr_t)(d + 0x20);
            *(float *)(uintptr_t)(d + 0x58) = 700.0f;   /* right side */
            *(float *)(uintptr_t)(d + 0x5C) = 300.0f;
            plog("  injecting CText def 0x%08lX id=%lu", d, g_text_id);
        }

        /* ATTACHMENT TEST. This fires after the definition is resolved and
         * before the component is built, so the child list can still be
         * changed. Append a duplicate of the first child to every screen
         * definition: if children really come from CUIDef+0x70, each screen
         * gains one extra element. */
        if (!seen && !IsBadReadPtr((void *)(uintptr_t)(d + TYPE_OFF), 4) &&
            (*(DWORD *)(uintptr_t)(d + TYPE_OFF) == 0x0A ||
             *(DWORD *)(uintptr_t)(d + TYPE_OFF) == 0x0C) &&
            !IsBadReadPtr((void *)(uintptr_t)(d + 0x78), 4)) {
            DWORD b = *(DWORD *)(uintptr_t)(d + 0x70);
            DWORD e = *(DWORD *)(uintptr_t)(d + 0x74);
            if (b > 0x10000 && e > b && (e - b) < 0x400) {
                DWORD n = (e - b) / 4;
                DWORD *nv = (DWORD *)VirtualAlloc(NULL, (n + 2) * 4,
                                MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
                if (nv) {
                    DWORD k;
                    for (k = 0; k < n; k++)
                        nv[k] = *(DWORD *)(uintptr_t)(b + k * 4);
                    if (g_text_id) { nv[n] = g_text_id; g_text_injected++; }
                    else            nv[n] = nv[n - 1];
                    *(DWORD *)(uintptr_t)(d + 0x70) = (DWORD)(uintptr_t)nv;
                    *(DWORD *)(uintptr_t)(d + 0x74) = (DWORD)(uintptr_t)(nv + n + 1);
                    *(DWORD *)(uintptr_t)(d + 0x78) = (DWORD)(uintptr_t)(nv + n + 1);
                    g_patched_screens++;
                }
            }
        }
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

/* Captured from a real factory invocation: `this` (a container holding a
 * definition manager at +0x64) and arg1, the definition reference the factory
 * resolves. Re-invoking with the same pair must build the same component. */
static volatile DWORD g_fac_this;
static volatile DWORD g_fac_arg1;
static unsigned char g_entry_orig;
static int g_entry_saved;

static LONG CALLBACK entry_handler(EXCEPTION_POINTERS *ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_BREAKPOINT &&
        (DWORD)(uintptr_t)ep->ExceptionRecord->ExceptionAddress == FACTORY_ENTRY) {
        DWORD prot;
        /* At the entry byte nothing is pushed yet: [esp] is the return
         * address and [esp+4] is arg1. */
        g_fac_this = ep->ContextRecord->Ecx;
        g_fac_arg1 = *(DWORD *)(uintptr_t)(ep->ContextRecord->Esp + 4);
        if (VirtualProtect((void *)(uintptr_t)FACTORY_ENTRY, 1,
                           PAGE_EXECUTE_READWRITE, &prot)) {
            *(unsigned char *)(uintptr_t)FACTORY_ENTRY = g_entry_orig;
            VirtualProtect((void *)(uintptr_t)FACTORY_ENTRY, 1, prot, &prot);
        }
        ep->ContextRecord->Eip = FACTORY_ENTRY;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void arm_entry_bp(void)
{
    DWORD prot;
    unsigned char cur = *(unsigned char *)(uintptr_t)FACTORY_ENTRY;
    if (cur == 0xCC) return;
    if (!g_entry_saved) { g_entry_orig = cur; g_entry_saved = 1; }
    if (VirtualProtect((void *)(uintptr_t)FACTORY_ENTRY, 1,
                       PAGE_EXECUTE_READWRITE, &prot)) {
        *(unsigned char *)(uintptr_t)FACTORY_ENTRY = 0xCC;
        VirtualProtect((void *)(uintptr_t)FACTORY_ENTRY, 1, prot, &prot);
    }
}

static DWORD call_this1(DWORD fn, DWORD thisp, DWORD a1)
{
    DWORD ret;
    __asm__ __volatile__(
        "pushl %[a]\n\t"
        "movl %[t], %%ecx\n\t"
        "call *%[f]"
        : "=a"(ret)
        : [f]"r"(fn), [t]"r"(thisp), [a]"r"(a1)
        : "ecx", "edx", "memory", "cc");
    return ret;
}

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


/*
 * Attachment.
 *
 * The child list is NOT the 4-byte vector at CFrontEndList+0x164 (that is the
 * ownership list; shrinking it never removed a rendered entry). The real one
 * holds 8-byte refcounted pairs { component*, int* refcount } in a standard
 * {begin,end,capacity} vector, and is appended to by:
 *
 *   0x00429C15  SmartPtr::SmartPtr(&tmp, component)   thiscall
 *   0x00535AD0  vector::push_back(vec, &tmp)          thiscall, needs capacity
 *   0x004291DE  SmartPtr::~SmartPtr(&tmp)             thiscall
 *
 * push_back only writes when end != capacity, so the vector is first relocated
 * into our own buffer with headroom.
 */
#define FN_SMARTPTR_CTOR 0x00429C15u
#define FN_VEC_PUSHBACK  0x00535AD0u
#define FN_SMARTPTR_DTOR 0x004291DEu

/* Find an 8-byte-stride run of pointers to any of `objs`; that run is a child
 * vector's storage. Returns the run start, and its length in elements. */
static DWORD find_child_storage(const DWORD *objs, int nobj, int *out_len)
{
    MEMORY_BASIC_INFORMATION mbi;
    unsigned char *addr = NULL;
    DWORD lo = 0xFFFFFFFFu, hi = 0;
    int i;

    for (i = 0; i < nobj; i++) {
        if (objs[i] < lo) lo = objs[i];
        if (objs[i] > hi) hi = objs[i];
    }
    while (VirtualQuery(addr, &mbi, sizeof mbi) == sizeof mbi) {
        unsigned char *next = (unsigned char *)mbi.BaseAddress + mbi.RegionSize;
        if (region_ok(&mbi)) {
            unsigned char *p = (unsigned char *)mbi.BaseAddress;
            unsigned char *end = next - 0x20;
            for (; p <= end; p += 4) {
                DWORD v0 = *(DWORD *)p;
                DWORD v1 = *(DWORD *)(p + 8);
                DWORD v2 = *(DWORD *)(p + 16);
                int m0 = 0, m1 = 0, m2 = 0;
                if (v0 < lo || v0 > hi) continue;
                for (i = 0; i < nobj; i++) {
                    if (objs[i] == v0) m0 = 1;
                    if (objs[i] == v1) m1 = 1;
                    if (objs[i] == v2) m2 = 1;
                }
                if (m0 && m1 && m2) {
                    int len = 0;
                    unsigned char *q = p;
                    while (q + 8 <= next) {
                        DWORD v = *(DWORD *)q;
                        int hit = 0;
                        for (i = 0; i < nobj; i++) if (objs[i] == v) hit = 1;
                        if (!hit) break;
                        len++; q += 8;
                    }
                    if (out_len) *out_len = len;
                    return (DWORD)(uintptr_t)p;
                }
            }
        }
        if (next <= addr) break;
        addr = next;
    }
    return 0;
}


/* Capture real attachments: INT3 on push_back records which vector each
 * component goes into, which beats guessing at strides in memory. */
#define MAX_ATTACH 32
static volatile DWORD g_att_vec[MAX_ATTACH];
static volatile DWORD g_att_comp[MAX_ATTACH];
static volatile LONG  g_att_n;
static unsigned char g_pb_orig;
static int g_pb_saved;

static LONG CALLBACK pushback_handler(EXCEPTION_POINTERS *ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_BREAKPOINT &&
        (DWORD)(uintptr_t)ep->ExceptionRecord->ExceptionAddress == FN_VEC_PUSHBACK) {
        DWORD prot;
        DWORD vec  = ep->ContextRecord->Ecx;
        DWORD pair = *(DWORD *)(uintptr_t)(ep->ContextRecord->Esp + 4);
        DWORD comp = 0;
        if (pair > 0x10000 && !IsBadReadPtr((void *)(uintptr_t)pair, 4))
            comp = *(DWORD *)(uintptr_t)pair;
        if (g_att_n < MAX_ATTACH) {
            g_att_vec[g_att_n] = vec;
            g_att_comp[g_att_n] = comp;
            g_att_n++;
        }
        if (VirtualProtect((void *)(uintptr_t)FN_VEC_PUSHBACK, 1,
                           PAGE_EXECUTE_READWRITE, &prot)) {
            *(unsigned char *)(uintptr_t)FN_VEC_PUSHBACK = g_pb_orig;
            VirtualProtect((void *)(uintptr_t)FN_VEC_PUSHBACK, 1, prot, &prot);
        }
        ep->ContextRecord->Eip = FN_VEC_PUSHBACK;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void arm_pushback_bp(void)
{
    DWORD prot;
    unsigned char cur = *(unsigned char *)(uintptr_t)FN_VEC_PUSHBACK;
    if (cur == 0xCC) return;
    if (!g_pb_saved) { g_pb_orig = cur; g_pb_saved = 1; }
    if (VirtualProtect((void *)(uintptr_t)FN_VEC_PUSHBACK, 1,
                       PAGE_EXECUTE_READWRITE, &prot)) {
        *(unsigned char *)(uintptr_t)FN_VEC_PUSHBACK = 0xCC;
        VirtualProtect((void *)(uintptr_t)FN_VEC_PUSHBACK, 1, prot, &prot);
    }
}


/* Rewrite the injected text to "Hello World", and push it right.
 *
 * Patterns are assembled at runtime so the scan cannot rewrite its own needle
 * (a literal here would be found and clobbered). The replacement is shorter
 * than the needle, so it is NUL-terminated in place rather than padded.
 * CUIDef+0x58 did not move the component, so the horizontal position must be a
 * runtime field; several candidates are written at once and narrowed later. */
static void say_one(const char *needle, int nlen);

static void say_hello(void)
{
    /* Which CText definition gets injected varies per run, so rewrite every
     * string we have seen it use. */
    static const char n1[] = {'T','h','e','r','e',' ','i','s',' ','a',' ','p','r','o','b','l','e','m',0};
    static const char n2[] = {'A','u','d','i','o',' ','O','p','t','i','o','n','s',0};
    static const char n3[] = {'V','i','d','e','o',' ','O','p','t','i','o','n','s',0};
    static const char n4[] = {'G','a','m','e',' ','O','p','t','i','o','n','s',0};
    static const char n5[] = {'R','e','d','e','f','i','n','e',' ','K','e','y','s',0};
    /* Only the long, distinctive needle. Short ones ("Game Options") match
     * ~20 places and rewriting them all during startup killed the game. */
    (void)n2; (void)n3; (void)n4; (void)n5;
    say_one(n1, 18);
}

static void say_one(const char *needle_in, int nlen_in)
{
    char needle[24], repl[16];
    unsigned short nw[24], rw[16];
    MEMORY_BASIC_INFORMATION mbi;
    unsigned char *addr = NULL;
    int i, nlen, rlen, hits = 0;

    nlen = nlen_in;
    for (i = 0; i < nlen; i++) needle[i] = needle_in[i];
    /* "Hello World" */
    rlen = 0;
    repl[rlen++]='H'; repl[rlen++]='e'; repl[rlen++]='l'; repl[rlen++]='l';
    repl[rlen++]='o'; repl[rlen++]=' '; repl[rlen++]='W'; repl[rlen++]='o';
    repl[rlen++]='r'; repl[rlen++]='l'; repl[rlen++]='d';

    for (i = 0; i < nlen; i++) nw[i] = (unsigned short)(unsigned char)needle[i];
    for (i = 0; i < rlen; i++) rw[i] = (unsigned short)(unsigned char)repl[i];

    while (VirtualQuery(addr, &mbi, sizeof mbi) == sizeof mbi) {
        unsigned char *next = (unsigned char *)mbi.BaseAddress + mbi.RegionSize;
        int writable = mbi.State == MEM_COMMIT &&
                       !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
                       !(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                                        PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) &&
                       (mbi.Protect & (PAGE_READWRITE | PAGE_READONLY | PAGE_WRITECOPY));
        if (writable && !(g_own_stack &&
              (unsigned char *)g_own_stack >= (unsigned char *)mbi.BaseAddress &&
              (unsigned char *)g_own_stack < next)) {
            unsigned char *q = (unsigned char *)mbi.BaseAddress;
            unsigned char *e2 = next - (nlen * 2) - 4;
            for (; q <= e2; q++) {
                DWORD prot, back;
                if (*q != (unsigned char)needle[0]) continue;
                if (memcmp(q, needle, nlen) == 0) {
                    if (VirtualProtect(q, nlen + 1, PAGE_READWRITE, &prot)) {
                        memcpy(q, repl, rlen);
                        q[rlen] = 0;
                        VirtualProtect(q, nlen + 1, prot, &back);
                        hits++;
                    }
                } else if (memcmp(q, nw, nlen * 2) == 0) {
                    if (VirtualProtect(q, nlen * 2 + 2, PAGE_READWRITE, &prot)) {
                        memcpy(q, rw, rlen * 2);
                        ((unsigned short *)q)[rlen] = 0;
                        VirtualProtect(q, nlen * 2 + 2, prot, &back);
                        hits++;
                    }
                }
            }
        }
        if (next <= addr) break;
        addr = next;
    }
    if (hits) plog("  say_hello: rewrote %d occurrence(s) of a %d-char needle", hits, nlen);
}

/*
 * Find the attach path.
 *
 * 0x0041DB4E is the instruction right after the wrapper's `call factory`, so at
 * that point EAX is the freshly built component and [EBP+4] is the wrapper's
 * own return address -- i.e. the caller that is about to do something with the
 * component. Whatever that caller does next IS the attach.
 */
#define WRAPPER_AFTER_FACTORY 0x0041DB4Eu

#define MAX_RET 16
static volatile DWORD g_ret[MAX_RET];
static volatile DWORD g_ret_comp[MAX_RET];
static volatile LONG  g_ret_n;
static unsigned char g_wr_orig;
static int g_wr_saved;

static LONG CALLBACK wrapper_handler(EXCEPTION_POINTERS *ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_BREAKPOINT &&
        (DWORD)(uintptr_t)ep->ExceptionRecord->ExceptionAddress == WRAPPER_AFTER_FACTORY) {
        DWORD prot;
        DWORD ebp = ep->ContextRecord->Ebp;
        DWORD ret = 0;
        LONG i;
        int seen = 0;
        if (ebp > 0x10000 && !IsBadReadPtr((void *)(uintptr_t)(ebp + 4), 4))
            ret = *(DWORD *)(uintptr_t)(ebp + 4);
        for (i = 0; i < g_ret_n; i++)
            if (g_ret[i] == ret) { seen = 1; break; }
        if (!seen && ret && g_ret_n < MAX_RET) {
            g_ret[g_ret_n] = ret;
            g_ret_comp[g_ret_n] = ep->ContextRecord->Eax;
            g_ret_n++;
        }
        if (VirtualProtect((void *)(uintptr_t)WRAPPER_AFTER_FACTORY, 1,
                           PAGE_EXECUTE_READWRITE, &prot)) {
            *(unsigned char *)(uintptr_t)WRAPPER_AFTER_FACTORY = g_wr_orig;
            VirtualProtect((void *)(uintptr_t)WRAPPER_AFTER_FACTORY, 1, prot, &prot);
        }
        ep->ContextRecord->Eip = WRAPPER_AFTER_FACTORY;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void arm_wrapper_bp(void)
{
    DWORD prot;
    unsigned char cur = *(unsigned char *)(uintptr_t)WRAPPER_AFTER_FACTORY;
    if (cur == 0xCC) return;
    if (!g_wr_saved) { g_wr_orig = cur; g_wr_saved = 1; }
    if (VirtualProtect((void *)(uintptr_t)WRAPPER_AFTER_FACTORY, 1,
                       PAGE_EXECUTE_READWRITE, &prot)) {
        *(unsigned char *)(uintptr_t)WRAPPER_AFTER_FACTORY = 0xCC;
        VirtualProtect((void *)(uintptr_t)WRAPPER_AFTER_FACTORY, 1, prot, &prot);
    }
}


#define VT_CTEXT 0x01249CCCu

/* Separate the duplicate we appended from its twin.
 *
 * A duplicated child inherits its twin's coordinates exactly, so the two draw
 * on top of each other. The vertical value appears mirrored at +0x038, +0x040
 * and +0x048 (stride 8), so the horizontal partner of each pair should be the
 * preceding dword: +0x034, +0x03C, +0x044. Shifting all three moves the copy
 * clear of the original. */
static DWORD g_shifted[128];
static int g_shifted_n;

static int already_shifted(DWORD c)
{
    int k;
    for (k = 0; k < g_shifted_n; k++)
        if (g_shifted[k] == c) return 1;
    if (g_shifted_n < 128) g_shifted[g_shifted_n++] = c;
    return 0;
}

static void spread_duplicates(void)
{
    DWORD texts[64];
    int n, i, j, moved = 0;

    n = scan_dword(VT_CTEXT, texts, 64);
    if (n > 64) n = 64;
    plog("");
    plog("=== separating duplicate text components (%d CText live) ===", n);

    for (i = 0; i < n; i++) {
        const char *ni = comp_defname(texts[i]);
        float yi;
        if (IsBadReadPtr((void *)(uintptr_t)(texts[i] + 0x48), 4)) continue;
        yi = *(float *)(uintptr_t)(texts[i] + 0x38);
        for (j = i + 1; j < n; j++) {
            const char *nj = comp_defname(texts[j]);
            float yj;
            if (IsBadReadPtr((void *)(uintptr_t)(texts[j] + 0x48), 4)) continue;
            yj = *(float *)(uintptr_t)(texts[j] + 0x38);
            /* Overlap is about POSITION, not identity: the extra child is a
             * different text component sitting at the same spot. Matching on
             * def name missed it entirely. */
            if (yi != yj) continue;
            (void)ni; (void)nj;
            if (already_shifted(texts[j])) break;   /* move once, never drift */

            /* same definition AND same vertical position: a duplicate */
            {
                /* +0x038 is the one offset proven to move a component on
                 * screen; the mirrors at +0x040/+0x048 move with it. */
                float *y1 = (float *)(uintptr_t)(texts[j] + 0x38);
                float *y2 = (float *)(uintptr_t)(texts[j] + 0x40);
                float *y3 = (float *)(uintptr_t)(texts[j] + 0x48);
                float was = *y1;
                *y1 = was + 150.0f;
                *y2 = was + 150.0f;
                *y3 = was + 150.0f;
                moved++;
                plog("  moved 0x%08lX (%s) y %.1f -> %.1f",
                     texts[j], nj ? nj : "?", (double)was, (double)*y1);
            }
            break;
        }
    }
    plog("  separated %d duplicate(s)", moved);
}



/* Dismiss Wine's video-error dialog from inside the process.
 *
 * macOS-level clicking needs accessibility permission that osascript does not
 * have here, but we are already injected -- so post the keystroke straight to
 * the game's own window instead. */
static void dismiss_dialogs(void)
{
    HWND hw = FindWindowA(NULL, "Fable - The Lost Chapters ");
    if (!hw) hw = FindWindowA(NULL, "Fable - The Lost Chapters");
    if (!hw) return;
    PostMessageA(hw, WM_KEYDOWN, VK_RETURN, 0);
    PostMessageA(hw, WM_KEYUP,   VK_RETURN, 0);
    PostMessageA(hw, WM_KEYDOWN, VK_SPACE, 0);
    PostMessageA(hw, WM_KEYUP,   VK_SPACE, 0);
}


/*
 * Inline hook on the factory's type dispatch -- deterministic, no racing.
 *
 * At 0x0041D249 the definition is resolved (EBX) but the component is not yet
 * built, which is the only window where a screen's child list can still be
 * changed. The bytes there are:
 *
 *   0041D249  8B 43 3C        mov eax,[ebx+0x3C]
 *   0041D24C  83 F8 2B        cmp eax,0x2B
 *
 * Six bytes, no relative operands, so a 5-byte jmp fits and the originals can
 * simply be re-executed in the stub. Flags from the cmp must survive, so the
 * register save/restore happens BEFORE it and we return straight to the ja.
 */
#define HOOK_SITE   0x0041D249u
#define HOOK_RESUME 0x0041D24Fu   /* the `ja` that follows the cmp */

void on_definition(unsigned int def);

/* Pre-allocated so the hook handler never calls into Win32. Allocating or
 * calling IsBadReadPtr from inside the game's own dispatch is the re-entrancy
 * that killed the process last time. */
#define POOL_SLOTS 64
#define POOL_ELEMS 64
static DWORD *g_pool;
static volatile LONG g_pool_next;

/* Plausible-pointer test that costs nothing and needs no API. The definition
 * is safe to read regardless: the very next instruction the game runs
 * dereferences it. */
#define PLAUSIBLE(p) ((p) >= 0x00400000u && (p) < 0x20000000u)

void on_definition(unsigned int def)
{
    DWORD b, e, n, k, *nv;
    LONG slot;

    if (!g_pool || !g_text_id) return;
    if (g_hook_hits >= 1) return;   /* one screen only: limit the blast radius */
    if (!PLAUSIBLE(def)) return;
    if (*(DWORD *)(uintptr_t)(def + TYPE_OFF) != 0x0A) return;   /* screens */

    b = *(DWORD *)(uintptr_t)(def + 0x70);
    e = *(DWORD *)(uintptr_t)(def + 0x74);
    if (!PLAUSIBLE(b) || e <= b || (e - b) > 0x200) return;
    n = (e - b) / 4;
    if (n + 1 >= POOL_ELEMS) return;

    for (k = 0; k < n; k++)                       /* already extended? */
        if (*(DWORD *)(uintptr_t)(b + k * 4) == g_text_id) return;

    slot = g_pool_next;
    if (slot >= POOL_SLOTS) return;
    g_pool_next = slot + 1;

    nv = g_pool + (DWORD)slot * POOL_ELEMS;
    for (k = 0; k < n; k++) nv[k] = *(DWORD *)(uintptr_t)(b + k * 4);
    nv[n] = g_text_id;

    *(DWORD *)(uintptr_t)(def + 0x70) = (DWORD)(uintptr_t)nv;
    *(DWORD *)(uintptr_t)(def + 0x74) = (DWORD)(uintptr_t)(nv + n + 1);
    *(DWORD *)(uintptr_t)(def + 0x78) = (DWORD)(uintptr_t)(nv + n + 1);
    g_hook_hits++;
}

/*
 * Hand-assembled trampoline.
 *
 * Relying on __attribute__((naked)) here was a mistake -- x86 GCC support for
 * it is unreliable, and a compiler-inserted prologue on this path corrupts the
 * stack and kills the game. Emitting the bytes directly removes the compiler
 * from the equation entirely.
 *
 *   60                pushad
 *   9C                pushfd
 *   53                push ebx              ; the definition
 *   B8 <on_definition>
 *   FF D0             call eax
 *   83 C4 04          add esp,4
 *   9D                popfd
 *   61                popad                 ; flags/regs restored BEFORE the cmp
 *   8B 43 3C          mov eax,[ebx+0x3C]    ; original
 *   83 F8 2B          cmp eax,0x2B          ; original -- sets flags for the ja
 *   68 <0x0041D24F>   push resume
 *   C3                ret
 */
static unsigned char *build_stub(void)
{
    unsigned char *p = (unsigned char *)VirtualAlloc(NULL, 64,
                          MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    int i = 0;
    if (!p) return NULL;
    p[i++] = 0x60;                                   /* pushad */
    p[i++] = 0x9C;                                   /* pushfd */
    p[i++] = 0x53;                                   /* push ebx */
    p[i++] = 0xB8; *(DWORD *)(p + i) = (DWORD)(uintptr_t)on_definition; i += 4;
    p[i++] = 0xFF; p[i++] = 0xD0;                    /* call eax */
    p[i++] = 0x83; p[i++] = 0xC4; p[i++] = 0x04;     /* add esp,4 */
    p[i++] = 0x9D;                                   /* popfd */
    p[i++] = 0x61;                                   /* popad */
    p[i++] = 0x8B; p[i++] = 0x43; p[i++] = 0x3C;     /* mov eax,[ebx+0x3C] */
    p[i++] = 0x83; p[i++] = 0xF8; p[i++] = 0x2B;     /* cmp eax,0x2B */
    p[i++] = 0x68; *(DWORD *)(p + i) = HOOK_RESUME; i += 4;
    p[i++] = 0xC3;                                   /* ret */
    return p;
}

static int install_inline_hook(void)
{
    DWORD prot;
    unsigned char *at = (unsigned char *)(uintptr_t)HOOK_SITE;
    unsigned char *stub = build_stub();
    int rel;
    if (!stub) return 0;
    rel = (int)((DWORD)(uintptr_t)stub - (HOOK_SITE + 5));
    if (!VirtualProtect(at, 8, PAGE_EXECUTE_READWRITE, &prot)) return 0;
    at[0] = 0xE9;                        /* jmp rel32 */
    *(int *)(at + 1) = rel;
    at[5] = 0x90;                        /* nop the 6th byte */
    VirtualProtect(at, 8, prot, &prot);
    return 1;
}
DWORD WINAPI probe_main(LPVOID unused)
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
    AddVectoredExceptionHandler(1, entry_handler);
    AddVectoredExceptionHandler(1, pushback_handler);
    AddVectoredExceptionHandler(1, wrapper_handler);
    plog("factory breakpoint at 0x%08X: %s", FACTORY_TYPE_READ,
         arm_factory_bp() ? "armed" : "FAILED");

    /* Inject the definition whose string we rewrite. The media-player-error
     * screen is id 604 and its child list begins at 605, so 605 is that
     * screen's text -- the one now reading "Hello World". */
    g_text_id = 605;
    /* Inline hook is correct in placement (5-byte jmp over a 6-byte,
     * relocation-free pair, resuming at the `ja`) but the handler calls
     * VirtualAlloc/IsBadReadPtr from inside the game's dispatch and that
     * re-entrancy kills it. Needs a pre-allocated buffer and no Win32 calls in
     * the handler before it can be enabled. */
    g_pool = (DWORD *)VirtualAlloc(NULL, POOL_SLOTS * POOL_ELEMS * 4,
                                   MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    plog("child-list pool: %s", g_pool ? "allocated" : "FAILED");
#ifdef ENABLE_INLINE_HOOK
    plog("inline hook at 0x%08X: %s", HOOK_SITE,
         install_inline_hook() ? "installed" : "FAILED");
#else
    /* Disabled: the trampoline still kills the game even patching a single
     * screen, so the fault is the hook itself, not the injection. Prime
     * suspect is the jmp rel32 -- VirtualAlloc can place the stub more than
     * 2GB from 0x0041D249, which silently overflows the displacement. Fix is
     * to allocate the stub near the image (VirtualAlloc with a preferred base
     * walking up from 0x00400000) and verify the delta fits in rel32. */
    (void)install_inline_hook;
    plog("inline hook: disabled pending rel32-range fix");
#endif

    /* Re-arm tightly: components are constructed in a burst, and each hit
     * disarms the breakpoint, so a slow cadence samples almost nothing. */
    for (t = 0; t < 60000 && g_caught_n < MAX_CAUGHT; t++) {
        arm_factory_bp();
        if (!g_fac_this) arm_entry_bp();
        arm_pushback_bp();
        arm_wrapper_bp();
        if ((t & 0x3F) == 0) say_hello();   /* before glyphs are baked */
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

        /* Structural check that does not depend on pixels: if the child list
         * drove construction, the menu now has one more child than the six it
         * ships with. Then offset the extra one so it is actually visible --
         * a duplicate inherits its twin's coordinates and would otherwise draw
         * exactly on top of it. */
        {
            int r;
            for (r = 0; r < 90; r++) { say_hello(); dismiss_dialogs(); Sleep(400); }
        }

        plog("");
        plog("=== ATTACHMENT RESULT ===");
        plog("  main menu children = %lu  (stock is 6)", count);
        if (count > 6) {
            DWORD extra = *(DWORD *)(uintptr_t)(begin + (count - 1) * 4);
            float *y = (float *)(uintptr_t)(extra + 0x38);
            if (!IsBadReadPtr(y, 4)) {
                float was = *y;
                *y = was + 30.0f;
                plog("  ATTACHED: extra child 0x%08lX (%s), y %.1f -> %.1f",
                     extra, comp_defname(extra) ? comp_defname(extra) : "?",
                     (double)was, (double)*y);
            }
        } else {
            plog("  no extra child -- the append did not reach this list");
        }

        plog("");
        plog("=== callers of the create wrapper (the attach sites) ===");
        plog("  distinct callers: %ld", g_ret_n);
        {
            LONG j;
            for (j = 0; j < g_ret_n && j < MAX_RET; j++) {
                DWORD c = g_ret_comp[j];
                DWORD vt = (c > 0x10000 && !IsBadReadPtr((void *)(uintptr_t)c, 4))
                           ? *(DWORD *)(uintptr_t)c : 0;
                plog("  [%2ld] returns to 0x%08lX   component=0x%08lX vt=0x%08lX",
                     j, g_ret[j], c, vt);
            }
        }

        plog("");
        plog("=== attachments observed (vector <- component) ===");
        plog("  captured %ld", g_att_n);
        {
            LONG j;
            for (j = 0; j < g_att_n && j < MAX_ATTACH; j++) {
                DWORD v = g_att_vec[j], c = g_att_comp[j];
                const char *nm = c ? comp_defname(c) : 0;
                DWORD vt = (c && !IsBadReadPtr((void *)(uintptr_t)c, 4))
                           ? *(DWORD *)(uintptr_t)c : 0;
                plog("  [%2ld] vec=0x%08lX (b=%08lX e=%08lX c=%08lX)  comp=0x%08lX vt=0x%08lX %s",
                     j, v,
                     IsBadReadPtr((void *)(uintptr_t)v, 12) ? 0 : *(DWORD *)(uintptr_t)v,
                     IsBadReadPtr((void *)(uintptr_t)v, 12) ? 0 : *(DWORD *)(uintptr_t)(v + 4),
                     IsBadReadPtr((void *)(uintptr_t)v, 12) ? 0 : *(DWORD *)(uintptr_t)(v + 8),
                     c, vt, nm ? nm : "");
                if (!g_child_vec) g_child_vec = v;
            }
        }

        plog("");
        plog("=== PROOF: build a component by calling the factory ===");
        plog("  captured this=0x%08lX arg1=0x%08lX", g_fac_this, g_fac_arg1);
        if (g_fac_this && g_fac_arg1) {
            DWORD comp = call_this1(FACTORY_ENTRY, g_fac_this, g_fac_arg1);
            plog("  factory returned component 0x%08lX", comp);
            if (comp && !IsBadReadPtr((void *)(uintptr_t)comp, 4)) {
                DWORD vt = *(DWORD *)(uintptr_t)comp;
                const char *nm = comp_defname(comp);
                plog("  component vtable = 0x%08lX", vt);
                plog("  component def    = %s", nm ? nm : "(none)");
                plog("  -> a natively constructed component");
            } else {
                plog("  !! factory returned nothing usable");
            }
        } else {
            plog("  (never caught a factory invocation)");
        }

        plog("");
        plog("=== definitions captured from the factory ===");
        plog("  distinct definitions: %ld", g_caught_n);
        plog("  INLINE HOOK fired on %ld screen definition(s)", g_hook_hits);
        plog("  INJECTED CText def=0x%08lX id=%lu (injected %ld time(s))",
             g_text_def, g_text_id, g_text_injected);
        plog("  screen child lists extended: %ld", g_patched_screens);
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
