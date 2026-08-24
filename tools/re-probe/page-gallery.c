/*
 * page-gallery: put one of every available widget kind on the Mods page.
 *
 * page-mods showed the page is ours to rebuild: a screen's contents are the
 * child-id list of its definition, and the factory builds whatever type each id
 * names. type-census then listed what is actually loaded -- definitions load
 * per screen, so this is bounded by where the game has been, and on the Options
 * page that is 17 distinct types.
 *
 * One of each goes into the Options screen's row list (219):
 *
 *     347  type 11  CFrontEndButton          a normal menu row
 *     494  type 16  CTextSlider              a slider
 *     478  type 15  (unnamed)                carries an action, has no children
 *                                            -- the shape of an on/off toggle
 *     622  type 18  (unnamed)
 *     317  type 22  CComponentContainer
 *     354  type 1   CMorphingSprite
 *
 * Laid out rather than left where they were authored. Each of these belongs to
 * some other screen and carries its own position, so dropped into one list they
 * would draw on top of each other. The row position is the float at
 * CUIStateDef[0] +0x4C -- established from the six menu buttons, whose values
 * are 0, 30, 60, 180, 210, 240, i.e. 30 per row -- so each gallery member is
 * given its own multiple of 30.
 *
 * Every widget here is a real definition the game already ships; nothing is
 * invented. Some may render oddly out of their home screen, and that is the
 * point of looking.
 *
 * All edits land in the factory window before construction, and the handler
 * only copies bytes -- a Win32 call from inside the game's dispatch deadlocks
 * it.
 */

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#define LOG_PATH "page-gallery.log"

#define DEF_ID          0x20
#define DEF_TYPE        0x3C
#define DEF_KEY         0x54
#define DEF_STATES      0x40
#define DEF_STATES_END  0x44
#define DEF_ACTION      0xC4
#define DEF_ACTION2     0xE4
#define DEF_CHILD_BEGIN 0x70
#define DEF_CHILD_END   0x74
#define DEF_CHILD_CAP   0x78

#define STATE_SIZE 0x7C
#define STATE_Y    0x4C          /* row position, 30 per row */

#define FACTORY_TYPE_READ 0x0041D249u
#define FACTORY_TYPE_LEN  3

#define QUIT_BUTTON 251
#define QUIT_CTEXT  254
#define ACTION_OPEN_PAGE 297

#define PAGE_TITLE 338
#define PAGE_LIST  219

static const WCHAR MENU_LABEL[] = L"Mods";
static const WCHAR TITLE_TEXT[] = L"Widgets";

/* id, and the row it should sit on */
#define GALLERY_N 6
static const DWORD GALLERY[GALLERY_N]  = { 347, 494, 478, 622, 317, 354 };
static const float ROW_Y[GALLERY_N]    = { 0.f, 30.f, 60.f, 90.f, 120.f, 150.f };

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

#define PLAUSIBLE(p) ((p) >= 0x00400000u && (p) < 0x20000000u)

static unsigned char g_orig;
static int g_saved;

static DWORD g_rows[GALLERY_N];          /* reserved: the handler allocates nothing */
static volatile LONG g_seen[GALLERY_N];
static volatile DWORD g_type[GALLERY_N];
static volatile LONG g_did_title, g_did_list, g_did_menu_text, g_did_menu_action;

static int arm_bp(void)
{
    DWORD prot;
    unsigned char cur = *(unsigned char *)(uintptr_t)FACTORY_TYPE_READ;
    if (cur == 0xCC) return 1;
    if (!g_saved) { g_orig = cur; g_saved = 1; }
    if (!VirtualProtect((void *)(uintptr_t)FACTORY_TYPE_READ, 1,
                        PAGE_EXECUTE_READWRITE, &prot)) return 0;
    *(unsigned char *)(uintptr_t)FACTORY_TYPE_READ = 0xCC;
    VirtualProtect((void *)(uintptr_t)FACTORY_TYPE_READ, 1, prot, &prot);
    return 1;
}

static void disarm_bp(void)
{
    DWORD prot;
    if (!g_saved) return;
    if (VirtualProtect((void *)(uintptr_t)FACTORY_TYPE_READ, 1,
                       PAGE_EXECUTE_READWRITE, &prot)) {
        *(unsigned char *)(uintptr_t)FACTORY_TYPE_READ = g_orig;
        VirtualProtect((void *)(uintptr_t)FACTORY_TYPE_READ, 1, prot, &prot);
    }
}

static int set_key(DWORD def, const WCHAR *text, SIZE_T bytes)
{
    DWORD strobj = *(DWORD *)(uintptr_t)(def + DEF_KEY);
    DWORD chars;
    if (!PLAUSIBLE(strobj)) return 0;
    chars = *(DWORD *)(uintptr_t)strobj;
    if (!PLAUSIBLE(chars)) return 0;
    memcpy((void *)(uintptr_t)chars, text, bytes);
    return 1;
}

/*
 * Laying the widgets out is now OPTIONAL, and off by default.
 *
 * With the position override on, the widgets drew on their own rows but none of
 * them could be interacted with. The obvious suspect is this write: +0x4C is
 * where the six menu buttons keep their row position, so it is certainly what
 * is DRAWN -- but if the clickable region is computed from somewhere else, then
 * moving this alone leaves every widget rendering in one place and listening in
 * another, which is exactly the symptom.
 *
 * Building with LAYOUT 0 changes one variable and nothing else. If the widgets
 * now respond but sit on top of each other, this write is the cause and the hit
 * region needs finding. If they are still dead while stacked at their authored
 * positions, position was never the problem and the list is.
 */
#define LAYOUT 0

static void set_row(DWORD def, float y)
{
    if (!LAYOUT) { (void)def; (void)y; return; }

    DWORD b = *(DWORD *)(uintptr_t)(def + DEF_STATES);
    DWORD e = *(DWORD *)(uintptr_t)(def + DEF_STATES_END);
    DWORD n, k;

    if (!PLAUSIBLE(b) || e <= b) return;
    n = (e - b) / STATE_SIZE;
    if (n > 16) return;
    for (k = 0; k < n; k++)
        memcpy((void *)(uintptr_t)(b + k * STATE_SIZE + STATE_Y), &y, 4);
}

static void edit(DWORD def)
{
    DWORD id = *(DWORD *)(uintptr_t)(def + DEF_ID);
    int i;

    if (id == QUIT_CTEXT && !g_did_menu_text)
        g_did_menu_text = set_key(def, MENU_LABEL, sizeof MENU_LABEL);

    if (id == QUIT_BUTTON && !g_did_menu_action) {
        *(DWORD *)(uintptr_t)(def + DEF_ACTION)  = ACTION_OPEN_PAGE;
        *(DWORD *)(uintptr_t)(def + DEF_ACTION2) = ACTION_OPEN_PAGE;
        g_did_menu_action = 1;
    }

    if (id == PAGE_TITLE && !g_did_title)
        g_did_title = set_key(def, TITLE_TEXT, sizeof TITLE_TEXT);

    if (id == PAGE_LIST && !g_did_list) {
        for (i = 0; i < GALLERY_N; i++) g_rows[i] = GALLERY[i];
        *(DWORD *)(uintptr_t)(def + DEF_CHILD_BEGIN) = (DWORD)(uintptr_t)g_rows;
        *(DWORD *)(uintptr_t)(def + DEF_CHILD_END)   =
            (DWORD)(uintptr_t)(g_rows + GALLERY_N);
        *(DWORD *)(uintptr_t)(def + DEF_CHILD_CAP)   =
            (DWORD)(uintptr_t)(g_rows + GALLERY_N);
        g_did_list = 1;
    }

    for (i = 0; i < GALLERY_N; i++) {
        if (id != GALLERY[i] || g_seen[i]) continue;
        g_type[i] = *(DWORD *)(uintptr_t)(def + DEF_TYPE);
        set_row(def, ROW_Y[i]);
        g_seen[i] = 1;
    }
}

static LONG CALLBACK bp_handler(EXCEPTION_POINTERS *ep)
{
    DWORD ebx;

    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT ||
        (DWORD)(uintptr_t)ep->ExceptionRecord->ExceptionAddress != FACTORY_TYPE_READ)
        return EXCEPTION_CONTINUE_SEARCH;

    ebx = ep->ContextRecord->Ebx;
    if (!PLAUSIBLE(ebx)) {
        disarm_bp();
        ep->ContextRecord->Eip = FACTORY_TYPE_READ;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    edit(ebx);

    ep->ContextRecord->Eax = *(DWORD *)(uintptr_t)(ebx + DEF_TYPE);
    ep->ContextRecord->Eip = FACTORY_TYPE_READ + FACTORY_TYPE_LEN;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void dismiss_dialogs(void)
{
    HWND hw = FindWindowA(NULL, "Fable - The Lost Chapters ");
    if (!hw) hw = FindWindowA(NULL, "Fable - The Lost Chapters");
    if (!hw) return;
    PostMessageA(hw, WM_KEYDOWN, VK_RETURN, 0);
    PostMessageA(hw, WM_KEYUP,   VK_RETURN, 0);
}

static DWORD WINAPI worker(LPVOID unused)
{
    int t, i;
    LONG said[GALLERY_N] = { 0 }, sT = 0, sL = 0, sM = 0;

    (void)unused;
    g_log = fopen(LOG_PATH, "w");
    plog("=== page-gallery: one of each widget kind on the Mods page ===");

    AddVectoredExceptionHandler(1, bp_handler);
    plog("breakpoint at 0x%08X: %s", FACTORY_TYPE_READ,
         arm_bp() ? "armed" : "FAILED");
    plog("");

    for (t = 0; t < 200000; t++) {
        if (g_did_menu_text && g_did_menu_action && !sM) {
            sM = 1; plog("  [menu]  row -> \"Mods\", action %d", ACTION_OPEN_PAGE);
        }
        if (g_did_title && !sT) { sT = 1; plog("  [page]  title -> \"Widgets\""); }
        if (g_did_list  && !sL) { sL = 1; plog("  [page]  list %d -> %d widgets",
                                              PAGE_LIST, GALLERY_N); }
        for (i = 0; i < GALLERY_N; i++)
            if (g_seen[i] && !said[i]) {
                said[i] = 1;
                plog("  [built] id %-5lu type %-3lu at row y=%d",
                     GALLERY[i], g_type[i], (int)ROW_Y[i]);
            }
        if ((t & 0x1F) == 0) dismiss_dialogs();
        Sleep(20);
    }
    disarm_bp();
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        CreateThread(NULL, 0, worker, NULL, 0, NULL);
    }
    return TRUE;
}
