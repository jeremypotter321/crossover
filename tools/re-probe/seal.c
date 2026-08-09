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

/* Absolute: when the probe is attached to an already-running game rather than
 * injected at startup, the process working directory is not the game folder,
 * and a relative fopen silently produced no log at all. */
#define LOG_PATH "C:\\Games\\Fable\\seal.log"

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

/*
 * GAME_ACTION_OPEN_IN_GAME_MENU is action 3, and 0x00687CF0 is the "was this
 * action pressed" query. Both handlers that answer action 3 -- 0x00690B20 and
 * 0x00691721 -- drop the press unless one bit is set on the hero:
 *
 *     test byte ptr [hero+0x38], 0x40
 *     je   <ignore the press>
 *
 * so with the bit clear the button does nothing at all, however much of the
 * seal the hero is carrying.
 */
#define HERO_MENU_FLAG_OFF  0x38u
#define HERO_MENU_FLAG_BIT  0x40u

/*
 * ...but that bit turned out to be set already on the tutorial hero. The gate
 * that actually stops the seal is in the CHARGE_GUILD_SEAL handler at
 * 0x0068E510, which -- once it has confirmed the hero is carrying the seal --
 * checks one byte on the script interface and gives up if it is zero:
 *
 *     call 0x005BDF08              ; carrying the seal?
 *     test eax, eax / jle <bail>
 *     mov  eax, [esi+0x14]         ; the CGameScriptInterface
 *     mov  cl, byte ptr [eax+0xD5]
 *     test cl, cl  / je <bail>     ; <-- using the seal silently does nothing
 *
 * Nothing else on the seal path reads it, so it reads as "the guild seal is
 * enabled" -- the switch the story throws when the seal stops being a prop.
 */
#define GSI_SEAL_ENABLED_OFF 0xD5u

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

/*
 * A one-shot INT3 at the instruction just past "is the hero carrying the seal",
 * inside the CHARGE_GUILD_SEAL handler. Reaching it proves the handler runs
 * when the seal is used, and captures ESI -- the handler object -- which is the
 * only way to reach the manager at [esi+0x14] whose +0xD5 byte gates the seal.
 * That object is NOT the CGameScriptInterface singleton: gsi+0xD5 reads 0x37,
 * which is data, not a flag.
 *
 * Guard pages never fire under Wine; INT3 plus a vectored handler does. It is
 * armed once and disarmed by the handler itself -- re-arming while already
 * armed saves 0xCC as the "original" byte and corrupts the restore.
 */
/*
 * NPlayerGui::CDrawGuildSeal holds the charge state. Its vtable has only two
 * entries, so the class is found by scanning for an object whose first dword is
 * the vtable -- read-only, no code patched, nothing called on the game thread.
 *
 *   +0x2C  id being charged, -1 when idle
 *   +0x30  charge timer (float), zeroed when the charge starts
 *   +0x34  charging flag
 *
 * Watching these settles the hold-vs-tap question outright: a tap should show
 * +0x34 go 1 and immediately back to 0 with +0x2C reset to -1, while a real
 * hold should show +0x30 climbing toward the seal's 2.0s charge time.
 */
#define CDRAWGUILDSEAL_VT 0x0125A6BCu

#define TRAP_ADDR 0x0068E76Bu

static volatile DWORD g_trap_hits, g_trap_esi, g_trap_eax, g_trap_ebx;
static unsigned char g_trap_orig;
static PVOID g_veh;

static LONG CALLBACK trap_handler(PEXCEPTION_POINTERS ep)
{
    /* Accept both EIP conventions. An x86 INT3 normally reports EIP just past
     * the 0xCC, but Wine has been seen to report it AT the trap byte, and the
     * first version of this only matched TRAP_ADDR+1 -- so the breakpoint fell
     * through to CONTINUE_SEARCH, nothing handled it, and the game died. */
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_BREAKPOINT &&
        (ep->ContextRecord->Eip == TRAP_ADDR + 1 ||
         ep->ContextRecord->Eip == TRAP_ADDR)) {
        /* Restore the byte in place -- no Win32 calls on the game's own
         * dispatch path; the page was left writable when it was armed. */
        *(unsigned char *)TRAP_ADDR = g_trap_orig;
        ep->ContextRecord->Eip = TRAP_ADDR;
        g_trap_esi = ep->ContextRecord->Esi;
        g_trap_eax = ep->ContextRecord->Eax;
        g_trap_ebx = ep->ContextRecord->Ebx;
        g_trap_hits++;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* Scan committed memory for an object whose first dword is the vtable. */
static DWORD find_seal_gui(void)
{
    MEMORY_BASIC_INFORMATION mbi;
    unsigned char *addr = NULL;

    while (VirtualQuery(addr, &mbi, sizeof mbi) == sizeof mbi) {
        unsigned char *next = (unsigned char *)mbi.BaseAddress + mbi.RegionSize;
        if (mbi.State == MEM_COMMIT &&
            !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
            (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE))) {
            unsigned char *p = (unsigned char *)mbi.BaseAddress;
            unsigned char *e = next - 0x40;
            for (; p <= e; p += 4)
                if (*(DWORD *)p == CDRAWGUILDSEAL_VT)
                    return (DWORD)(uintptr_t)p;
        }
        if (next <= addr) break;
        addr = next;
    }
    return 0;
}

static void watch_charge(DWORD gui, DWORD *last_id, DWORD *last_flag, float *last_t)
{
    DWORD id, flag;
    float t;

    if (!readable(gui + 0x34)) return;
    id   = rd(gui + 0x2C);
    t    = *(float *)(uintptr_t)(gui + 0x30);
    flag = *(unsigned char *)(uintptr_t)(gui + 0x34);

    if (id != *last_id || flag != *last_flag ||
        (t > *last_t + 0.05f) || (t < *last_t - 0.05f)) {
        plog("    charge: id=%ld  timer=%d.%03d  charging=%lu",
             (long)id, (int)t, (int)((t - (int)t) * 1000), flag);
        *last_id = id; *last_flag = flag; *last_t = t;
    }
}

static void arm_trap(void)
{
    DWORD old;
#ifndef ENABLE_SEAL_TRAP
    /*
     * OFF by default, and it stays that way. This crashed a live session, and
     * with no save games a crash costs the whole tutorial -- which cannot be
     * re-saved either, because saving is behind the very menu being debugged.
     * Build with -DENABLE_SEAL_TRAP only when that cost is acceptable.
     */
    plog("  INT3 trap disabled (build with -DENABLE_SEAL_TRAP to arm it)");
    return;
#endif
    if (!VirtualProtect((void *)TRAP_ADDR, 1, PAGE_EXECUTE_READWRITE, &old)) {
        plog("  could not make 0x%08X writable: %lu", TRAP_ADDR, GetLastError());
        return;
    }
    g_trap_orig = *(unsigned char *)TRAP_ADDR;
    if (g_trap_orig == 0xCC) {
        plog("  0x%08X is already 0xCC -- refusing to arm over an armed trap",
             TRAP_ADDR);
        return;
    }
    g_veh = AddVectoredExceptionHandler(1, trap_handler);
    if (!g_veh) {
        plog("  AddVectoredExceptionHandler failed");
        return;
    }
    *(unsigned char *)TRAP_ADDR = 0xCC;
    /* Deliberately left PAGE_EXECUTE_READWRITE: the handler restores the byte
     * and must not call VirtualProtect from the dispatch path. EXECUTE is kept,
     * so the page stays fetchable. */
    plog("  armed INT3 at 0x%08X (original byte 0x%02X) -- now use the seal",
         TRAP_ADDR, g_trap_orig);
}

/*
 * Hold the in-game-menu bit set. It is done in a loop rather than once because
 * the tutorial may well clear it again every frame -- and if it does, the log
 * says so instead of leaving us guessing why one write "did not take".
 * The hero is re-resolved each pass so a level transition does not leave this
 * writing to a freed object.
 */
static void hold_menu_flag(DWORD gsi)
{
    DWORD last = 0xFFFFFFFF, cleared = 0, seal_off = 0;
    int reported = 0;
    DWORD gui = 0, c_id = 0xFFFFFFFE, c_flag = 0xFF;
    float c_t = -1.0f;
    int i;

    plog("");
    plog("holding hero+0x%X bit 0x%02X and gsi+0x%X set",
         HERO_MENU_FLAG_OFF, HERO_MENU_FLAG_BIT, GSI_SEAL_ENABLED_OFF);
    plog("  gsi+0x%X reads 0x%02X right now",
         GSI_SEAL_ENABLED_OFF,
         readable(gsi + GSI_SEAL_ENABLED_OFF)
             ? *(unsigned char *)(uintptr_t)(gsi + GSI_SEAL_ENABLED_OFF) : 0xFF);
    arm_trap();

    gui = find_seal_gui();
    if (gui)
        plog("  CDrawGuildSeal @0x%08lX -- watching the charge state.\n"
             "  TAP the seal, then HOLD it for 3 seconds.", gui);
    else
        plog("  CDrawGuildSeal not found (vtable 0x%08X)", CDRAWGUILDSEAL_VT);

    for (i = 0; i < 7200; i++) {          /* ~30 min at 250 ms */
        DWORD hero = find_hero(gsi);
        unsigned char *p, v;

        /* Report the trap the moment the seal handler runs. */
        if (g_trap_hits && !reported) {
            DWORD mgr = rd(g_trap_esi + 0x14);
            reported = 1;
            plog("");
            plog("  *** 0x%08X reached -- the seal handler DID run ***", TRAP_ADDR);
            plog("      esi(handler)=0x%08lX  eax=0x%08lX  ebx=0x%08lX",
                 g_trap_esi, g_trap_eax, g_trap_ebx);
            plog("      [esi+0x14] = 0x%08lX   (the manager the gate hangs off)", mgr);
            if (readable(mgr + GSI_SEAL_ENABLED_OFF))
                plog("      [esi+0x14]+0x%X = 0x%02X  -> the seal is %s",
                     GSI_SEAL_ENABLED_OFF,
                     *(unsigned char *)(uintptr_t)(mgr + GSI_SEAL_ENABLED_OFF),
                     *(unsigned char *)(uintptr_t)(mgr + GSI_SEAL_ENABLED_OFF)
                         ? "ENABLED" : "DISABLED -- this is the blocker");
            else
                plog("      [esi+0x14]+0x%X unreadable", GSI_SEAL_ENABLED_OFF);
        }

        if (gui) watch_charge(gui, &c_id, &c_flag, &c_t);

        if (!hero) { Sleep(250); continue; }
        p = (unsigned char *)(uintptr_t)(hero + HERO_MENU_FLAG_OFF);
        if (!readable(hero + HERO_MENU_FLAG_OFF)) { Sleep(250); continue; }

        v = *p;
        if (v != last) {
            DWORD f34 = rd(hero + 0x34);
            plog("  t=%ds  hero 0x%08lX  +0x38=0x%02X (menu bit %s)  "
                 "+0x34=%08lX (bit 0x20000 %s)  +0x30=%08lX +0x3C=%08lX",
                 i / 4, hero, v, (v & HERO_MENU_FLAG_BIT) ? "SET" : "clear",
                 f34, (f34 & 0x20000) ? "set" : "CLEAR",
                 rd(hero + 0x30), rd(hero + 0x3C));
            last = v;
        }
        if (!(v & HERO_MENU_FLAG_BIT)) {
            *p = (unsigned char)(v | HERO_MENU_FLAG_BIT);
            cleared++;
            last = *p;
        }
        Sleep(100);
    }
    plog("  the game cleared the menu bit %lu time(s), and the seal-enabled\n        byte %lu time(s), while we held them", cleared, seal_off);
}

/*
 * Watch-only: pure memory reads, no call into game code at all.
 *
 * Everything that calls into the game -- the give, the container queries, even
 * resolving the hero -- runs on the injected thread while the game thread is
 * mid-frame. That raced and killed a live session: the log stopped exactly at
 * "calling GiveHeroObject" with no return. The first few times it survived were
 * luck, not correctness.
 *
 * Nothing here needs those calls. The hero already carries the seal, so the
 * give was never necessary, and the charge state can be read straight out of
 * CDrawGuildSeal. Reads cannot corrupt the game; at worst they see a torn value
 * for one sample.
 */
static DWORD WINAPI watch_only(void)
{
    DWORD gui = 0, c_id = 0xFFFFFFFE, c_flag = 0xFF;
    float c_t = -1.0f;
    int i;

    plog("=== seal: watch-only (no calls into game code) ===");
    plog("waiting for CDrawGuildSeal (vtable 0x%08X)", CDRAWGUILDSEAL_VT);

    for (i = 0; i < 18000; i++) {           /* ~30 min at 100 ms */
        if (!gui) {
            if (rd(GSI_PTR)) gui = find_seal_gui();
            if (gui) {
                plog("");
                plog("CDrawGuildSeal @0x%08lX", gui);
                plog("  id=-1 means idle. TAP the seal, then HOLD it 3 seconds.");
            } else if (i % 100 == 0) {
                plog("t=%3ds  not found yet", i / 10);
            }
        } else {
            watch_charge(gui, &c_id, &c_flag, &c_t);
        }
        Sleep(100);
    }
    plog("=== watch complete ===");
    return 0;
}

static DWORD WINAPI probe_main(LPVOID unused)
{
    int t, announced = 0;
    (void)unused;

    g_log = fopen(LOG_PATH, "w");
    if (!g_log) return 0;

#ifndef ENABLE_GIVE
    watch_only();
    fclose(g_log);
    g_log = NULL;
    return 0;
#endif

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
        hold_menu_flag(gsi);
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
