/*
 * seal: give the guild seal to the hero, the way the game's own story quest
 * `Q_GuildTrainingDeparture` does it, and then report where it landed.
 *
 * The route was recovered statically (see docs/guild-seal.md). In short:
 *
 *   gsi   = *(void **)0x0143E8F8            CGameScriptInterface singleton
 *   vt    = *(void ***)gsi                  must be 0x01260F0C
 *   gsi->GiveHeroObject(&CharString name, -1, 1)   == vt[0x1E4/4], __thiscall
 *
 * GiveHeroObject resolves the name to a definition id through the definition
 * manager at gsi+0x10, finds the hero through gsi+0x14, and calls the give
 * primitive on it -- so passing the name is all that is needed.
 *
 * Giving the object is only half of it. The HUD (and the guild seal menu that
 * hangs off it) does not ask "is the seal in the inventory" in general; at
 * 0x0064A75C it asks specifically whether the hero owns **container slot
 * 0x11**, and then whether the container in that slot holds the seal:
 *
 *   hero+0x20   bitset of container slots the hero owns   (test: 0x00410DE0)
 *   hero+0x44   std::map<int slot, container *>           (find: 0x004365B0)
 *   count       0x005BDF08(container, defId)
 *
 * A child/tutorial hero may simply not own slot 0x11, in which case the give
 * is a silent no-op as far as the seal UI is concerned. So the probe reports
 * that state either side of the give rather than assuming the give worked.
 */

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>

#define LOG_PATH "probe.log"

/* --- verified absolute addresses (Fable.exe, ImageBase 0x400000, no ASLR) --- */
#define GSI_PTR        0x0143E8F8u  /* CGameScriptInterface *                  */
#define GSI_PTR_ALT    0x0143E8F0u  /* same object, written by the same ctor    */
#define GSI_VTABLE     0x01260F0Cu  /* .?AVCGameScriptInterface@@               */
#define VT_GIVE_OBJECT 0x1E4u       /* vtable byte offset of GiveHeroObject     */
#define FN_CHARSTR_CTOR 0x0099EBF0u /* __thiscall(CharString*, const char*, int)*/
#define FN_CHARSTR_DTOR 0x0099EAE0u /* __thiscall(CharString*)                  */
#define FN_NAME_TO_DEFID 0x009AD410u/* __thiscall(defmgr, CharString*) -> int   */
#define FN_WORLD_GET    0x00449970u /* __thiscall(gsi+0x14) -> world            */
#define FN_HERO_GET     0x00487DC0u /* __thiscall(world) -> hero CThing*        */
#define FN_SLOT_OWNED   0x00410DE0u /* __thiscall(hero+0x20, int slot) -> bool  */
#define FN_SLOT_FIND    0x004365B0u /* __thiscall(hero+0x44, int *slot) -> pair**/
#define FN_COUNT_OF     0x005BDF08u /* __thiscall(container, int defId) -> int  */
#define VT_SET_HUD      0x594u      /* vtable offset of HUD(bool)               */

#define SEAL_NAME "OBJECT_GUILD_SEAL_1"
#define SEAL_SLOT 0x11              /* the slot the HUD checks for the seal     */

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

/* --- thiscall thunks. GCC has no __thiscall, so ECX is loaded by hand. --- */

static void charstr_ctor(void *cs, const char *s)
{
    __asm__ __volatile__(
        "pushl $-1\n\t"        /* length sentinel, as the game passes  */
        "pushl %1\n\t"
        "movl %0, %%ecx\n\t"
        "call *%2\n\t"
        :
        : "r"(cs), "r"(s), "r"((void *)FN_CHARSTR_CTOR)
        : "eax", "ecx", "edx", "memory");
}

static void charstr_dtor(void *cs)
{
    __asm__ __volatile__(
        "movl %0, %%ecx\n\t"
        "call *%1\n\t"
        :
        : "r"(cs), "r"((void *)FN_CHARSTR_DTOR)
        : "eax", "ecx", "edx", "memory");
}

/* __thiscall f(this) -> eax */
static DWORD thiscall0(DWORD fn, void *self)
{
    DWORD ret;
    __asm__ __volatile__(
        "movl %1, %%ecx\n\t"
        "call *%2\n\t"
        : "=a"(ret)
        : "r"(self), "r"(fn)
        : "ecx", "edx", "memory");
    return ret;
}

/* __thiscall f(this, void *a) -> eax  (callee cleans 4 bytes) */
static DWORD thiscall1(DWORD fn, void *self, void *a)
{
    DWORD ret;
    __asm__ __volatile__(
        "pushl %3\n\t"
        "movl %1, %%ecx\n\t"
        "call *%2\n\t"
        : "=a"(ret)
        : "r"(self), "r"(fn), "r"(a)
        : "ecx", "edx", "memory");
    return ret;
}

/* __thiscall f(this, void *a, int b, int c)  (callee cleans 12 bytes) */
static DWORD thiscall3(DWORD fn, void *self, void *a, int b, int c)
{
    DWORD ret;
    __asm__ __volatile__(
        "pushl %5\n\t"
        "pushl %4\n\t"
        "pushl %3\n\t"
        "movl %1, %%ecx\n\t"
        "call *%2\n\t"
        : "=a"(ret)
        : "r"(self), "r"(fn), "r"(a), "r"(b), "r"(c)
        : "ecx", "edx", "memory");
    return ret;
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

/*
 * Resolve the hero the same way GiveHeroObject does, so a NULL here means the
 * give would have been a no-op anyway.
 */
static DWORD find_hero(DWORD gsi)
{
    DWORD world_holder = rd(gsi + 0x14), world, hero;
    if (!readable(world_holder)) return 0;
    world = thiscall0(FN_WORLD_GET, (void *)(uintptr_t)world_holder);
    if (!readable(world)) return 0;
    hero = thiscall0(FN_HERO_GET, (void *)(uintptr_t)world);
    if (!readable(hero)) return 0;
    /* the game rejects a hero with bit 0 of +0x91 set (dead/invalid) */
    if (*(unsigned char *)(uintptr_t)(hero + 0x91) & 1) {
        plog("  hero 0x%08lX rejected: [+0x91] & 1 set", hero);
        return 0;
    }
    return hero;
}

/*
 * Report the seal-relevant part of the hero exactly the way the HUD reads it:
 * does the hero own container slot 0x11, and does that container hold `defid`.
 */
static void dump_state(const char *when, DWORD hero, DWORD defid)
{
    DWORD tree = hero + 0x44, head, pair, container;
    int slot = SEAL_SLOT, owned, i;

    plog("  --- hero state %s ---", when);

    plog("    slot bitset @hero+0x20: %08lX %08lX %08lX %08lX",
         rd(hero + 0x20), rd(hero + 0x24), rd(hero + 0x28), rd(hero + 0x2C));

    owned = (int)thiscall1(FN_SLOT_OWNED, (void *)(uintptr_t)(hero + 0x20),
                           (void *)(uintptr_t)SEAL_SLOT) & 0xFF;
    plog("    owns container slot 0x%02X: %s", SEAL_SLOT, owned ? "yes" : "NO");

    head = rd(tree + 4);
    pair = thiscall1(FN_SLOT_FIND, (void *)(uintptr_t)tree, &slot);
    if (!pair || pair == head || (readable(pair) && (int)rd(pair) > SEAL_SLOT)) {
        plog("    container slot 0x%02X: not present in the map", SEAL_SLOT);
        return;
    }
    container = rd(pair + 4);
    plog("    container slot 0x%02X = 0x%08lX", SEAL_SLOT, container);
    if (readable(container))
        plog("    count of def %ld in it: %ld", (long)defid,
             (long)thiscall1(FN_COUNT_OF, (void *)(uintptr_t)container,
                             (void *)(uintptr_t)defid));

    /* Context: what else the hero carries, so an empty slot is distinguishable
     * from a slot the probe simply cannot read. */
    for (i = 0; i < 0x20; i++) {
        int s = i;
        DWORD p, c;
        if (s == SEAL_SLOT) continue;
        p = thiscall1(FN_SLOT_FIND, (void *)(uintptr_t)tree, &s);
        if (!p || p == head || !readable(p) || (int)rd(p) != s) continue;
        c = rd(p + 4);
        if (readable(c))
            plog("      (slot 0x%02X container 0x%08lX)", s, c);
    }
}

static int give_seal(DWORD gsi, DWORD hero)
{
    DWORD vt = rd(gsi), fn, defid = 0, defmgr;
    void *cs = NULL;   /* CharString is one pointer wide */

    if (vt != GSI_VTABLE) {
        plog("  vtable 0x%08lX != expected 0x%08X -- refusing to call", vt, GSI_VTABLE);
        return 0;
    }
    fn = rd(vt + VT_GIVE_OBJECT);
    if (!readable(fn)) {
        plog("  vtable+0x%X is not callable", VT_GIVE_OBJECT);
        return 0;
    }

    charstr_ctor(&cs, SEAL_NAME);
    plog("  CharString(\"%s\") -> 0x%08lX", SEAL_NAME, (DWORD)(uintptr_t)cs);

    /* Same lookup the give does first: a defid <= 0 means it would no-op. */
    defmgr = rd(gsi + 0x10);
    if (readable(defmgr)) {
        defid = thiscall1(FN_NAME_TO_DEFID, (void *)(uintptr_t)defmgr, &cs);
        plog("  definition id = %ld", (long)defid);
        if ((long)defid <= 0) {
            plog("  name did not resolve -- not calling");
            charstr_dtor(&cs);
            return 0;
        }
    } else {
        plog("  definition manager at gsi+0x10 unreadable; calling blind");
    }

    dump_state("before give", hero, defid);

    /* (name, -1, 1) is exactly what Q_GuildTrainingDeparture passes at
     * 0x00D489B5 when the story hands the hero the seal. */
    plog("  calling GiveHeroObject @0x%08lX (name, -1, 1) ...", fn);
    thiscall3(fn, (void *)(uintptr_t)gsi, &cs, -1, 1);
    plog("  returned without faulting");
    charstr_dtor(&cs);

    Sleep(500);
    dump_state("after give", hero, defid);

    /* The seal menu hangs off the HUD, and the tutorial runs with the HUD off
     * (the `HUD` script command drives this same vtable slot). Turn it on. */
    if (readable(rd(vt + VT_SET_HUD))) {
        plog("  enabling HUD via vtable+0x%X", VT_SET_HUD);
        thiscall1(rd(vt + VT_SET_HUD), (void *)(uintptr_t)gsi, (void *)1);
        plog("  HUD call returned");
    }
    return 1;
}

static DWORD WINAPI probe_main(LPVOID unused)
{
    int t, announced = 0;
    (void)unused;

    g_log = fopen(LOG_PATH, "w");
    if (!g_log) return 0;

    plog("=== seal: give \"%s\" via CGameScriptInterface ===", SEAL_NAME);
    plog("waiting for a loaded game (singleton + world + hero)");

    for (t = 0; t < 4000; t++) {      /* ~33 min at 500 ms */
        DWORD gsi = rd(GSI_PTR), hero;

        if (!gsi) {
            if (t % 40 == 0)
                plog("t=%4ds  singleton NULL (alt 0x%08lX)", t / 2, rd(GSI_PTR_ALT));
            Sleep(500);
            continue;
        }
        if (!announced) {
            plog("");
            plog("t=%ds  CGameScriptInterface @0x%08lX  vtable 0x%08lX",
                 t / 2, gsi, rd(gsi));
            plog("        defmgr(+0x10)=0x%08lX  world(+0x14)=0x%08lX",
                 rd(gsi + 0x10), rd(gsi + 0x14));
            announced = 1;
        }

        hero = find_hero(gsi);
        if (!hero) {
            if (t % 40 == 0) plog("t=%4ds  no hero yet", t / 2);
            Sleep(500);
            continue;
        }

        plog("");
        plog("t=%ds  HERO @0x%08lX", t / 2, hero);
        if (give_seal(gsi, hero))
            plog("=== seal given ===");
        else
            plog("=== give did not run ===");
        fclose(g_log);
        g_log = NULL;
        return 0;
    }

    plog("=== timed out: no hero appeared ===");
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
